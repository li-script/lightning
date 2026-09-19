#include <algorithm>
#include <deque>
#include <ir/bc2ir.hpp>
#include <ir/insn.hpp>
#include <ir/proc.hpp>
#include <ir/runtime.hpp>
#include <ir/value.hpp>
#include <iterator>
#include <lib/std.hpp>
#include <limits>
#include <optional>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <vm/tier.hpp>

namespace li::ir {
	static std::optional<type> strict_ir_type(const strict::typespec& spec) {
		switch (spec.kind) {
			case strict::type_kind::sized_int:
				return strict::dynamic_storage(spec);
			case strict::type_kind::f32:
				return type::f64;
			case strict::type_kind::f64:
			case strict::type_kind::number:
				return strict::dynamic_storage(spec);
			case strict::type_kind::typed_array:
			case strict::type_kind::fixed_array:
				return type::tarr;
			default:
				return std::nullopt;
		}
	}

	static std::optional<size_t> strict_parameter_index(bc::reg slot) {
		if (slot > -FRAME_SIZE - 1)
			return std::nullopt;
		const auto index = size_t(-slot - FRAME_SIZE - 1);
		return index < MAX_ARGS ? std::optional<size_t>{index} : std::nullopt;
	}

	static vclass* strict_struct_array_class(function_proto* f, bc::reg slot) {
		auto parameter = strict_parameter_index(slot);
		if (!parameter || !f->signature || *parameter >= f->signature->parameters.size())
			return nullptr;
		const auto& spec = f->signature->parameters[*parameter];
		if (spec.kind != strict::type_kind::typed_array || spec.children.size() != 1)
			return nullptr;
		const auto& element = spec.children.front();
		return element.kind == strict::type_kind::struct_ref ? element.declared : nullptr;
	}

	static bool may_throw(bc::opcode op) {
		switch (op) {
			case bc::ANEG:
			case bc::AADD:
			case bc::ASUB:
			case bc::AMUL:
			case bc::ADIV:
			case bc::AMOD:
			case bc::APOW:
			case bc::CCAT:
			case bc::CEQ:
			case bc::CNE:
			case bc::CLT:
			case bc::CGE:
			case bc::CGT:
			case bc::CLE:
			case bc::FDUP:
			case bc::SETEX:
			case bc::STRIV:
			case bc::SGET:
			case bc::SSET:
			case bc::TGET:
			case bc::TSET:
			case bc::TGETR:
			case bc::TSETR:
			case bc::USET:
			case bc::VAGET:
			case bc::VACHK:
			case bc::CALL:
			case bc::ITER:
			case bc::RET:
				return true;
			default:
				return false;
		}
	}

	static bool has_normal_fallthrough(bc::opcode op) { return op != bc::RET && op != bc::JMP; }

	static void lift_basic_block(builder bld, const std::vector<basic_block*>& bc_to_bb, const std::vector<int32_t>& handler_at,
		 const std::vector<int32_t>& cleanup_at, const std::vector<int32_t>& pending_at, const std::vector<bool>& materialized_locals,
		 const std::vector<bool>& in_loop, basic_block* exception_return) {
		function_proto*         f = bld.blk->proc->f;
		std::vector<ref<value>> local_locals;
		local_locals.resize(f->num_locals + MAX_ARGS + FRAME_SIZE);
		constexpr bc::reg local_shift = MAX_ARGS + FRAME_SIZE;

		auto get_kval        = [&](bc::reg r) { return f->kvals()[r]; };
		auto is_materialized = [&](bc::reg r) {
			const bc::reg slot = r + local_shift;
			return slot >= 0 && size_t(slot) < materialized_locals.size() && materialized_locals[size_t(slot)];
		};
		auto load_reg = [&](builder& at, bc::reg r) -> ref<value> {
			auto load         = at.emit<load_local>(r);
			load->is_volatile = is_materialized(r);
			auto parameter    = strict_parameter_index(r);
			if (!parameter || !f->signature || *parameter >= f->signature->parameters.size())
				return load;
			if (at.current_bc < f->length) {
				const auto& source = f->opcode_array[at.current_bc];
				if ((source.o == bc::CTY || source.o == bc::CTYX) && source.b == r)
					return load;
			}
			auto seeded = strict_ir_type(f->signature->parameters[*parameter]);
			return seeded && *seeded != type::any ? at.emit<assume_cast>(std::move(load), *seeded) : std::move(load);
		};
		auto store_reg = [&](builder& at, bc::reg r, ref<value> value) {
			auto store         = at.emit<store_local>(r, std::move(value));
			store->is_volatile = is_materialized(r);
		};
		// Emit every bytecode assignment at its source position. PHI lifting
		// removes ordinary stores but consumes them as exact slot-lifetime
		// boundaries; stores to materialized frame registers remain physical.
		auto set_reg = [&]<typename T>(bc::reg r, T&& v) {
			auto value                    = launder_value(bld.blk->proc, std::forward<T>(v));
			local_locals[r + local_shift] = value;
			store_reg(bld, r, std::move(value));
		};
		auto get_reg = [&](bc::reg r) -> ref<value> {
			auto& x = local_locals[r + local_shift];
			if (!x) {
				x = load_reg(bld, r);
				if (r == FRAME_TARGET)
					x = bld.emit<assume_cast>(std::move(x), type::fn);
			}
			return x;
		};
		auto this_func      = [&]() { return get_reg(FRAME_TARGET); };
		auto is_cached_load = [](value* source, bc::reg r) {
			while (source->is<insn>()) {
				auto* instruction = source->as<insn>();
				if (instruction->is<load_local>())
					return instruction->operands[0]->as<constant>()->i32 == r;
				if (!instruction->is<assume_cast>() && !instruction->is<move>() && !instruction->is<erase_type>())
					return false;
				source = instruction->operands[0].get();
			}
			return false;
		};
		auto spill = [&]() {
			for (size_t slot = 0; slot != local_locals.size(); ++slot) {
				auto& x = local_locals[slot];
				if (!x)
					continue;
				auto r = bc::reg(slot) - local_shift;
				if (is_cached_load(x.get(), r))
					continue;
				store_reg(bld, r, x);
			}
		};
		auto exception_target = [&]() {
			auto handler = handler_at[bld.current_bc];
			return handler >= 0 ? bc_to_bb[size_t(handler)] : exception_return;
		};
		// Every failing edge records the throwing bytecode position before joining
		// the handler; sites sharing a position share the cold block.
		std::unordered_map<bc::pos, basic_block*> located_failures;
		auto                                      locate_failure = [&](basic_block* target) {
			auto [it, inserted] = located_failures.try_emplace(bld.current_bc, nullptr);
			if (inserted) {
				auto* locate      = bld.blk->proc->add_block();
				locate->cold_hint = 1;
				builder cold{locate};
				cold.current_bc = bld.current_bc;
				cold.emit<ccall>(&exception_location_info, 0, int32_t(bld.current_bc));
				cold.emit<jmp>(target);
				bld.blk->proc->add_jump(locate, target);
				it->second = locate;
			}
			return it->second;
		};
		basic_block* checked_failure          = nullptr;
		auto         checked_exception_target = [&]() {
			auto handler = handler_at[bld.current_bc];
			auto cleanup = cleanup_at[bld.current_bc];
			if (cleanup < 0)
				return locate_failure(handler >= 0 ? bc_to_bb[size_t(handler)] : exception_return);
			if (cleanup == handler)
				return locate_failure(bc_to_bb[size_t(cleanup)]);
			if (checked_failure)
				return locate_failure(checked_failure);

			auto* normal_target  = handler >= 0 ? bc_to_bb[size_t(handler)] : exception_return;
			auto* cleanup_target = bc_to_bb[size_t(cleanup)];
			checked_failure      = bld.blk->proc->add_block();
			builder dispatch{checked_failure};
			dispatch.current_bc = bld.current_bc;
			auto forced         = dispatch.emit<ccall>(&runtime::forced_unwind_info, 0);
			dispatch.emit<jcc>(forced, cleanup_target, normal_target);
			bld.blk->proc->add_jump(checked_failure, cleanup_target);
			bld.blk->proc->add_jump(checked_failure, normal_target);
			return locate_failure(checked_failure);
		};
		auto branch_checked = [&](ref<insn> result, std::optional<bc::reg> destination, bc::pos continuation) {
			auto  ok      = bld.emit<extract>(result, int32_t(1));
			auto* success = bc_to_bb[continuation];
			if (destination) {
				success = bld.blk->proc->add_block();
				builder commit{success};
				commit.current_bc = bld.current_bc;
				store_reg(commit, *destination, commit.emit<extract>(result, int32_t(0)));
				commit.emit<jmp>(bc_to_bb[continuation]);
				bld.blk->proc->add_jump(success, bc_to_bb[continuation]);
			}
			auto* failure = checked_exception_target();
			bld.emit<jcc>(ok, success, failure);
			bld.blk->proc->add_jump(bld.blk, success);
			bld.blk->proc->add_jump(bld.blk, failure);
		};

		int32_t pending_depth = pending_at[bld.blk->bc_begin];
		LI_ASSERT(pending_depth >= 0);
		std::vector<ref<value>> pending_values;
		pending_values.reserve(size_t(pending_depth));
		for (int32_t n = 0; n != pending_depth; ++n)
			pending_values.emplace_back(load_reg(bld, bc::reg(f->num_locals + n)));

		auto specialize_loop_numbers = [&](std::vector<ref<value>>& operands, bc::pos resume_ip) {
			if (!in_loop[resume_ip] || pending_depth != 0)
				return false;
			bool has_unknown = false;
			for (const auto& operand : operands) {
				if (operand->vt == type::any) {
					has_unknown = true;
				} else if (!is_integer_data(operand->vt) && !is_floating_point_data(operand->vt)) {
					return false;
				}
			}
			if (!has_unknown)
				return false;

			std::vector<ref<value>> locals;
			locals.reserve(f->num_locals);
			for (bc::reg r = 0; r != bc::reg(f->num_locals); ++r)
				locals.emplace_back(get_reg(r));

			ref<value> condition;
			for (const auto& operand : operands) {
				if (operand->vt != type::any)
					continue;
				auto numeric = bld.emit<test_type>(operand, type_number);
				condition    = condition ? bld.emit<bool_and>(condition, numeric) : numeric;
			}

			auto* success      = bld.blk->proc->add_block();
			auto* failure      = bld.blk->proc->add_block();
			failure->cold_hint = 1;
			builder fail{failure};
			fail.current_bc                 = resume_ip;
			auto  exit                      = fail.emit<deopt>(int32_t(resume_ip));
			auto* side_exit                 = exit->as<deopt>();
			side_exit->exception_handler_pc = handler_at[resume_ip] < 0 ? no_interpreter_handler : uint32_t(handler_at[resume_ip]);
			side_exit->cleanup_handler_pc   = cleanup_at[resume_ip] < 0 ? no_interpreter_handler : uint32_t(cleanup_at[resume_ip]);
			for (bc::reg r = 0; r != bc::reg(f->num_locals); ++r) {
				side_exit->operands.emplace_back(bld.blk->proc->add_const(int32_t(r)));
				side_exit->operands.emplace_back(locals[size_t(r)]);
			}
			side_exit->update();

			bld.emit<jcc>(condition, success, failure);
			bld.blk->proc->add_jump(bld.blk, success);
			bld.blk->proc->add_jump(bld.blk, failure);
			bld            = builder{success};
			bld.current_bc = resume_ip;
			for (auto& operand : operands) {
				if (operand->vt == type::any)
					operand = bld.emit<assume_cast>(operand, type::f64);
			}
			return true;
		};

		bc::pos ip      = bld.blk->bc_begin;
		bc::pos ip_end  = bld.blk->bc_end;
		auto    opcodes = f->opcodes();
		while (ip != ip_end) {
			bld.current_bc      = ip;
			auto& insn          = opcodes[ip++];
			auto& [op, a, b, c] = insn;

			switch (op) {
				case bc::NOP:
					continue;

				case bc::LNOT:
					set_reg(a, bld.emit<bool_xor>(bld.emit<coerce_bool>(get_reg(b)), true));
					continue;
				case bc::ANEG: {
					std::vector<ref<value>> operands{get_reg(b)};
					specialize_loop_numbers(operands, ip - 1);
					spill();
					auto result = bld.emit<unop>(op, operands[0]);
					branch_checked(std::move(result), a, ip);
					return;
				}

				case bc::AADD:
				case bc::ASUB:
				case bc::AMUL:
				case bc::ADIV:
				case bc::AMOD:
				case bc::APOW: {
					std::vector<ref<value>> operands{get_reg(b), get_reg(c)};
					specialize_loop_numbers(operands, ip - 1);
					spill();
					auto result = bld.emit<binop>(op, operands[0], operands[1]);
					branch_checked(std::move(result), a, ip);
					return;
				}

				case bc::KIMM:
					set_reg(a, any(std::in_place, insn.xmm()));
					continue;
				case bc::MOV:
					set_reg(a, get_reg(b));
					continue;

				case bc::LAND: {
					auto lhs = get_reg(b);
					set_reg(a, bld.emit<select>(bld.emit<coerce_bool>(lhs), get_reg(c), lhs));
					continue;
				}
				case bc::LOR: {
					auto lhs = get_reg(b);
					set_reg(a, bld.emit<select>(bld.emit<coerce_bool>(lhs), lhs, get_reg(c)));
					continue;
				}
				case bc::NCS: {
					auto       lhs = get_reg(b);
					ref<value> is_nil;
					if (lhs->vt == type::any)
						is_nil = bld.emit<test_type>(lhs, type_nil);
					else
						is_nil = launder_value(bld.blk->proc, lhs->vt == type::nil);
					set_reg(a, bld.emit<select>(is_nil, get_reg(c), lhs));
					continue;
				}
				case bc::CTY:
					set_reg(a, bld.emit<test_type>(get_reg(b), value_type(c)));
					continue;
				case bc::CTYX:
					set_reg(a, bld.emit<class_is>(get_reg(b), get_reg(c)));
					continue;
				case bc::CTYID:
					set_reg(
						 a, bld.emit<ccall>(&runtime::class_matches_info, int32_t(0), get_reg(b), li::bit_cast<int64_t>(bld.blk->proc->f->return_class_identity)));
					continue;
				case bc::CEQ:
				case bc::CNE:
				case bc::CGT:
				case bc::CGE:
				case bc::CLE:
				case bc::CLT: {
					std::vector<ref<value>> operands{get_reg(b), get_reg(c)};
					specialize_loop_numbers(operands, ip - 1);
					spill();
					auto result = bld.emit<compare>(op, operands[0], operands[1]);
					branch_checked(std::move(result), a, ip);
					return;
				}

				case bc::ANEW:
					bld.emit<safepoint>();
					set_reg(a, bld.emit<array_new>(b));
					continue;
				case bc::TNEW:
					bld.emit<safepoint>();
					set_reg(a, bld.emit<table_new>(b));
					continue;
				case bc::CCAT: {
					std::vector<ref<value>> values;
					values.reserve(b);
					for (msize_t n = 0; n != b; ++n)
						values.emplace_back(get_reg(a + n));
					spill();
					bld.emit<safepoint>();

					ref<value> joined;
					for (auto value : values) {
						if (value->vt != type::str) {
							if (value->vt != type::any)
								value = bld.emit<erase_type>(std::move(value));
							auto  conversion = bld.emit<ccall>(&lib::detail::builtin_str.nfi, 1, std::move(value));
							auto  ok         = bld.emit<extract>(conversion, int32_t(1));
							auto* success    = bld.blk->proc->add_block();
							auto* failure    = checked_exception_target();
							bld.emit<jcc>(ok, success, failure);
							bld.blk->proc->add_jump(bld.blk, success);
							bld.blk->proc->add_jump(bld.blk, failure);
							bld            = builder{success};
							bld.current_bc = ip - 1;
							value          = bld.emit<assume_cast>(bld.emit<extract>(conversion, int32_t(0)), type::str);
						}
						if (joined) {
							joined = bld.emit<ccall>(&lib::detail::builtin_join.nfi, 2, std::move(joined), std::move(value));
						} else {
							joined = std::move(value);
						}
					}
					store_reg(bld, a, joined);
					bld.emit<jmp>(bc_to_bb[ip]);
					bld.blk->proc->add_jump(bld.blk, bc_to_bb[ip]);
					return;
				}

				case bc::TOBOOL:
					set_reg(a, bld.emit<coerce_bool>(get_reg(b)));
					continue;

				case bc::FDUP: {
					auto                    bf = get_kval(b);
					std::vector<ref<value>> captures;
					captures.reserve(bf.as_fn()->num_uval);
					for (bc::reg n = 0; n != bf.as_fn()->num_uval; ++n)
						captures.emplace_back(get_reg(c + n));
					spill();
					bld.emit<safepoint>();

					int32_t dup_overload  = -1;
					auto    dup_overloads = lib::detail::builtin_dup.nfi.get_overloads();
					for (size_t n = 0; n != dup_overloads.size(); ++n) {
						if (dup_overloads[n].args.size() == 1 && dup_overloads[n].args[0] == type::fn) {
							dup_overload = int32_t(n);
							break;
						}
					}
					LI_ASSERT(dup_overload >= 0);
					auto result = bld.emit<ccall>(&lib::detail::builtin_dup.nfi, dup_overload, bf);
					result      = bld.emit<assume_cast>(result, type::fn);
					for (size_t n = 0; n != captures.size(); ++n) {
						auto  status  = bld.emit<ccall>(&runtime::capture_set_info, 0, result, int32_t(n), captures[n]);
						auto  ok      = bld.emit<extract>(status, int32_t(1));
						auto* success = bld.blk->proc->add_block();
						auto* failure = checked_exception_target();
						bld.emit<jcc>(ok, success, failure);
						bld.blk->proc->add_jump(bld.blk, success);
						bld.blk->proc->add_jump(bld.blk, failure);
						bld            = builder{success};
						bld.current_bc = ip - 1;
					}
					store_reg(bld, a, result);
					bld.emit<jmp>(bc_to_bb[ip]);
					bld.blk->proc->add_jump(bld.blk, bc_to_bb[ip]);
					return;
				}
				case bc::UGET:
					if (f->shared)
						set_reg(a, bld.emit<ccall>(&runtime::capture_get_info, 0, this_func(), int32_t(b)));
					else
						set_reg(a, bld.emit<uval_get>(this_func(), b));
					continue;
				case bc::USET: {
					auto          target = this_func();
					auto          value  = get_reg(b);
					ref<ir::insn> status;
					spill();
					if (f->shared)
						status = bld.emit<ccall>(&runtime::capture_set_info, 0, target, int32_t(a), value);
					else
						status = bld.emit<uval_set>(target, a, value);
					branch_checked(std::move(status), std::nullopt, ip);
					return;
				}

				case bc::STRIV: {
					auto cl = get_reg(FRAME_SELF);
					spill();
					auto result = bld.emit<class_new>(cl);
					branch_checked(std::move(result), a, ip);
					return;
				}
				case bc::TGET:
				case bc::TGETR:
				case bc::SGET: {
					auto    object        = get_reg(c);
					auto    key           = get_reg(b);
					vclass* element_class = op == bc::TGET ? strict_struct_array_class(f, c) : nullptr;
					if (element_class && object->vt == type::any)
						object = bld.emit<assume_cast>(object, type::tarr);
					spill();
					auto result = element_class ? bld.emit<struct_array_get>(object, key, any(element_class)) : bld.emit<field_get>(op != bc::TGET, object, key);
					branch_checked(std::move(result), a, ip);
					return;
				}
				case bc::TSET:
				case bc::TSETR:
				case bc::SSET: {
					auto object = get_reg(c);
					auto key    = get_reg(a);
					auto value  = get_reg(b);
					spill();
					auto result = bld.emit<field_set>(op != bc::TSET, object, key, value);
					branch_checked(std::move(result), std::nullopt, ip);
					return;
				}

				case bc::PUSHI: {
					auto value = launder_value(bld.blk->proc, any(std::in_place, insn.xmm()));
					store_reg(bld, bc::reg(f->num_locals + pending_depth++), value);
					pending_values.emplace_back(std::move(value));
					continue;
				}
				case bc::PUSHR: {
					auto value = get_reg(a);
					store_reg(bld, bc::reg(f->num_locals + pending_depth++), value);
					pending_values.emplace_back(std::move(value));
					continue;
				}
				case bc::CALL: {
					auto consumed = int32_t(b + 2);
					LI_ASSERT(consumed >= 2 && pending_depth >= consumed);
					auto first = pending_depth - consumed;
					LI_ASSERT(pending_values.size() == size_t(pending_depth));
					std::vector<ref<value>> call_args;
					call_args.reserve(size_t(consumed));
					for (auto it = pending_values.begin() + first; it != pending_values.end(); ++it)
						call_args.emplace_back(std::move(*it));
					pending_values.resize(size_t(first));
					pending_depth = first;

					bld.blk->proc->max_stack_slot = std::max<msize_t>(msize_t(call_args.size() + 1), bld.blk->proc->max_stack_slot);
					spill();
					auto call = bld.emit<vcall>(std::move(*call_args.rbegin()), std::move(*std::next(call_args.rbegin())));
					call->operands.insert(
						 call->operands.end(), std::make_move_iterator(std::next(call_args.rbegin(), 2)), std::make_move_iterator(call_args.rend()));
					branch_checked(std::move(call), a, ip);
					return;
				}

				case bc::SETEH:
					continue;
				case bc::SETEX: {
					auto value = get_reg(a);
					spill();
					auto status = bld.emit<set_exception>(value);
					branch_checked(std::move(status), std::nullopt, ip);
					return;
				}
				case bc::GETEX:
					set_reg(a, bld.emit<get_exception>());
					continue;

				case bc::VACNT:
					set_reg(a, bld.emit<va_count>());
					continue;
				case bc::VAGET: {
					auto index = get_reg(b);
					spill();
					auto result = bld.emit<va_get>(index);
					branch_checked(std::move(result), a, ip);
					return;
				}
				case bc::VACHK: {
					spill();
					auto    count   = bld.emit<va_count>();
					auto    enough  = bld.emit<compare>(bc::CGE, count, int32_t(a));
					auto*   failure = bld.blk->proc->add_block();
					builder fail{failure};
					fail.current_bc = bld.current_bc;
					fail.emit<set_exception>(any(std::in_place, insn.xmm()));
					fail.emit<jmp>(exception_target());
					bld.blk->proc->add_jump(failure, exception_target());
					bld.emit<jcc>(enough, bc_to_bb[ip], failure);
					bld.blk->proc->add_jump(bld.blk, bc_to_bb[ip]);
					bld.blk->proc->add_jump(bld.blk, failure);
					return;
				}

				case bc::RET: {
					auto result = get_reg(a);
					if (result->vt == type::any && f->signature) {
						auto seeded = strict_ir_type(f->signature->return_type);
						if (seeded && *seeded != type::any)
							result = bld.emit<assume_cast>(result, *seeded);
					}
					if (result->vt != type::any && result->vt != type::exc) {
						bld.emit<ret>(result);
						return;
					}

					auto* failure = checked_exception_target();
					if (result->vt == type::exc) {
						bld.emit<jmp>(failure);
						bld.blk->proc->add_jump(bld.blk, failure);
						return;
					}

					auto* success = bld.blk->proc->add_block();
					builder{success}.emit<ret>(result);
					auto ok = bld.emit<extract>(result, int32_t(1));
					bld.emit<jcc>(ok, success, failure);
					bld.blk->proc->add_jump(bld.blk, success);
					bld.blk->proc->add_jump(bld.blk, failure);
					return;
				}
				case bc::JS:
				case bc::JNS: {
					auto* false_target = bc_to_bb[ip];
					auto* true_target  = bc_to_bb[ip + a];
					if (op == bc::JNS)
						std::swap(true_target, false_target);
					spill();
					auto condition = get_reg(b);
					if (!condition->is(type::i1))
						condition = bld.emit<coerce_bool>(condition);
					bld.emit<jcc>(condition, true_target, false_target);
					bld.blk->proc->add_jump(bld.blk, true_target);
					bld.blk->proc->add_jump(bld.blk, false_target);
					return;
				}
				case bc::JMP: {
					auto* target = bc_to_bb[ip + a];
					spill();
					bld.emit<jmp>(target);
					bld.blk->proc->add_jump(bld.blk, target);
					return;
				}
				case bc::ITER: {
					auto target = get_reg(c);
					spill();
					auto  status   = bld.emit<iter_next>(target, int32_t(b));
					auto  ok       = bld.emit<extract>(status, int32_t(1));
					auto* classify = bld.blk->proc->add_block();
					auto* failure  = checked_exception_target();
					bld.emit<jcc>(ok, classify, failure);
					bld.blk->proc->add_jump(bld.blk, classify);
					bld.blk->proc->add_jump(bld.blk, failure);

					builder next{classify};
					next.current_bc = bld.current_bc;
					auto found      = next.emit<coerce_bool>(next.emit<extract>(status, int32_t(0)));
					next.emit<jcc>(found, bc_to_bb[ip], bc_to_bb[ip + a]);
					bld.blk->proc->add_jump(classify, bc_to_bb[ip]);
					bld.blk->proc->add_jump(classify, bc_to_bb[ip + a]);
					return;
				}
				default:
					util::abort("Opcode %s NYI\n", bc::opcode_details(op).name);
			}
		}

		auto* target = bc_to_bb[ip];
		LI_ASSERT(target);
		spill();
		bld.emit<jmp>(target);
		bld.blk->proc->add_jump(bld.blk, target);
	}

	std::unique_ptr<procedure> lift_bc(vm* L, function_proto* f, std::optional<bc::pos> osr_target) {
		auto                      proc = std::make_unique<procedure>(L, f);
		std::vector<basic_block*> bc_to_bb(f->length + 1);
		// ITER publishes its state, key, and value through the VM frame. Keep
		// those registers memory-backed throughout the procedure so SSA lifting
		// cannot substitute values that preceded a call to the iterator helper.
		std::vector<bool> materialized_locals(f->num_locals + MAX_ARGS + FRAME_SIZE);
		std::vector<bool> in_loop(f->length, false);
		constexpr bc::reg local_shift = MAX_ARGS + FRAME_SIZE;
		for (bc::pos ip = 0; ip != f->length; ++ip) {
			const auto& insn = f->opcode_array[ip];
			if (insn.o == bc::ITER) {
				for (bc::reg r = insn.b; r != insn.b + 3; ++r)
					materialized_locals[r + local_shift] = true;
			}
		}

		auto add_label = [&](bc::pos ip) {
			LI_ASSERT(ip < f->length);
			auto*& block = bc_to_bb[ip];
			if (!block) {
				block           = proc->add_block();
				block->bc_begin = ip;
			}
		};
		add_label(0);
		for (bc::pos i = 0; i != f->length; ++i) {
			auto  ip   = i + 1;
			auto& insn = f->opcode_array[i];
			if (insn.o == bc::JMP) {
				add_label(ip + insn.a);
			} else if (insn.o == bc::JS || insn.o == bc::JNS || insn.o == bc::ITER) {
				add_label(ip);
				add_label(ip + insn.a);
			} else if (insn.o == bc::SETEH) {
				if (insn.a)
					add_label(ip + insn.a);
				if (insn.c)
					add_label(ip + insn.c);
			}
			if (may_throw(insn.o) && has_normal_fallthrough(insn.o) && ip < f->length)
				add_label(ip);
		}

		constexpr int32_t    state_unknown = std::numeric_limits<int32_t>::min();
		constexpr int32_t    handler_none  = -1;
		std::vector<int32_t> handler_at(f->length, state_unknown);
		std::vector<int32_t> cleanup_at(f->length, state_unknown);
		std::vector<int32_t> pending_at(f->length, state_unknown);
		std::deque<bc::pos>  worklist;
		handler_at[0] = handler_none;
		cleanup_at[0] = handler_none;
		pending_at[0] = 0;
		worklist.push_back(0);
		auto propagate = [&](bc::pos to, int32_t handler, int32_t cleanup, int32_t pending) {
			LI_ASSERT(to < f->length && pending >= 0);
			if (handler_at[to] == state_unknown) {
				handler_at[to] = handler;
				cleanup_at[to] = cleanup;
				pending_at[to] = pending;
				worklist.push_back(to);
			} else {
				LI_ASSERT(pending_at[to] == pending);
				// Handler entry blocks restore all three SETEH operands before
				// executing user cleanup/catch code, so their incoming handler
				// state may legitimately differ across nested protected scopes.
				if (f->opcode_array[to].o != bc::SETEH) {
					LI_ASSERT(handler_at[to] == handler);
					LI_ASSERT(cleanup_at[to] == cleanup);
				}
			}
		};
		while (!worklist.empty()) {
			auto at = worklist.front();
			worklist.pop_front();
			auto& insn             = f->opcode_array[at];
			auto  next             = at + 1;
			auto  handler          = handler_at[at];
			auto  cleanup          = cleanup_at[at];
			auto  pending          = pending_at[at];
			auto  outgoing_handler = handler;
			auto  outgoing_cleanup = cleanup;
			auto  outgoing_pending = pending;
			if (insn.o == bc::SETEH) {
				outgoing_handler = insn.a ? int32_t(next + insn.a) : handler_none;
				outgoing_cleanup = insn.c ? int32_t(next + insn.c) : handler_none;
			}
			if (insn.o == bc::PUSHI || insn.o == bc::PUSHR) {
				++outgoing_pending;
			} else if (insn.o == bc::CALL) {
				auto consumed = int32_t(insn.b + 2);
				LI_ASSERT(consumed >= 2 && outgoing_pending >= consumed);
				outgoing_pending -= consumed;
			}

			if (may_throw(insn.o)) {
				if (handler >= 0)
					propagate(bc::pos(handler), handler, cleanup, 0);
				if (cleanup >= 0 && cleanup != handler)
					propagate(bc::pos(cleanup), handler, cleanup, 0);
			}
			switch (insn.o) {
				case bc::RET:
					break;
				case bc::JMP:
					propagate(next + insn.a, outgoing_handler, outgoing_cleanup, outgoing_pending);
					break;
				case bc::JS:
				case bc::JNS:
				case bc::ITER:
					propagate(next, outgoing_handler, outgoing_cleanup, outgoing_pending);
					propagate(next + insn.a, outgoing_handler, outgoing_cleanup, outgoing_pending);
					break;
				default:
					if (next < f->length)
						propagate(next, outgoing_handler, outgoing_cleanup, outgoing_pending);
					break;
			}
		}
		for (size_t n = 0; n != handler_at.size(); ++n) {
			if (handler_at[n] == state_unknown) {
				handler_at[n] = handler_none;
				cleanup_at[n] = handler_none;
				pending_at[n] = 0;
			}
		}

		std::vector<basic_block*> bytecode_blocks;
		bytecode_blocks.reserve(proc->basic_blocks.size());
		for (auto& block : proc->basic_blocks)
			bytecode_blocks.push_back(block.get());
		for (auto* block : bytecode_blocks) {
			bc::pos end = block->bc_begin + 1;
			while (end < f->length && !bc_to_bb[end])
				++end;
			block->bc_end = end;
		}

		auto* exception_return = proc->add_block();
		builder{exception_return}.emit<ret>(any(exception_marker));
		for (auto* block : bytecode_blocks)
			lift_basic_block(block, bc_to_bb, handler_at, cleanup_at, pending_at, materialized_locals, in_loop, exception_return);

		// The OSR entry adopts every owning local of the interpreter frame and
		// continues at the requested loop header. Statement boundaries have no
		// pending call arguments, so the header's incoming state is the locals.
		//
		if (osr_target && *osr_target < f->length && bc_to_bb[*osr_target] && pending_at[*osr_target] == 0) {
			auto* header    = bc_to_bb[*osr_target];
			auto* original  = proc->get_entry();
			auto* dispatch  = proc->add_block();
			auto* adopt     = proc->add_block();
			adopt->bc_begin = adopt->bc_end = *osr_target;

			builder check{dispatch};
			check.current_bc = 0;
			auto taken       = check.emit<ccall>(&tier::osr_entry_info, 0, int32_t(*osr_target));
			check.emit<jcc>(taken, adopt, original);
			proc->add_jump(dispatch, adopt);
			proc->add_jump(dispatch, original);

			builder transfer{adopt};
			transfer.current_bc = *osr_target;
			for (bc::reg r = 0; r != bc::reg(f->num_locals); ++r)
				transfer.emit<store_local>(r, transfer.emit<osr_load>(r));
			transfer.emit<jmp>(header);
			proc->add_jump(adopt, header);

			auto entry_it = std::find_if(proc->basic_blocks.begin(), proc->basic_blocks.end(), [&](auto& block) { return block.get() == dispatch; });
			std::rotate(proc->basic_blocks.begin(), entry_it, std::next(entry_it));
			proc->osr_target = *osr_target;
		}

		proc->remove_unreachable_blocks();
		proc->topological_sort();
		return proc;
	}
};
