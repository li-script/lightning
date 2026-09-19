#include <array>
#include <cmath>
#include <lang/parser.hpp>
#include <lib/std.hpp>
#include <limits>
#include <span>
#include <util/user.hpp>
#include <util/utf.hpp>
#include <variant>
#include <vm/array.hpp>
#include <vm/atomic.hpp>
#include <vm/function.hpp>
#include <vm/object.hpp>
#include <vm/runtime.hpp>
#include <vm/shared.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <vm/tier.hpp>
#include <vm/traits.hpp>

namespace li {
	generic_template::~generic_template() {
		for (auto& candidate : candidates)
			rc::release(L, candidate.attributes);
		for (auto& record : instantiations)
			rc::release(L, record.value);
		for (vclass* forward : recursive_forwards)
			rc::release(L, forward);
		rc::release(L, name);
	}

	namespace {
		any_t LI_CC parser_field_delete(vm* L, any* args, slot_t count) {
			if (count != 2)
				return L->error("invalid delete call");
			return runtime::field_delete(L, args[0], args[-1]);
		}

		util::native_function delete_field_function = {
			 func_attr_sideeffect | func_attr_c_takes_vm,
			 nullptr,
			 &parser_field_delete,
		};

		any_t LI_CC parser_class_of(vm* L, any* args, slot_t count) {
			if (count != 1 || !args[0].is_obj() || !args[0].as_obj()->cl)
				return L->error("class_of expects an object instance");
			return L->ok(any(args[0].as_obj()->cl));
		}

		util::native_function class_of_function = {
			 func_attr_c_takes_vm,
			 nullptr,
			 &parser_class_of,
		};

		struct continuation_scope {
			func_state& fn;

			explicit continuation_scope(func_state& fn) : fn(fn) { ++fn.continuation_depth; }
			~continuation_scope() { --fn.continuation_depth; }
		};

		bool can_continue_expression(const func_scope& scope) {
			return scope.fn.continuation_depth != 0 || scope.fn.lex.tok.source_line <= scope.fn.lex.last_token_line;
		}

		expression immutable_module_function(func_scope& scope, std::string_view module_name, string* member_name) {
			auto* module_key = scope.fn.own(string::create(scope.fn.L, module_name));
			any   module     = scope.fn.L->modules->get(scope.fn.L, any(module_key));
			if (!module.is_tbl() || !module.as_tbl()->is_frozen) {
				scope.lex().error("required immutable module '%.*s' is unavailable", int(module_name.size()), module_name.data());
				return {};
			}

			any value = module.as_tbl()->get(scope.fn.L, any(member_name));
			if (!value.is_fn()) {
				scope.lex().error("required function '%.*s.%s' is unavailable", int(module_name.size()), module_name.data(), member_name->c_str());
				return {};
			}
			return expression(value);
		}

		expression emit_ordinary_call(func_scope& scope, const expression& callee, std::span<const expression> args) {
			bc::reg result = scope.alloc_reg(2);
			for (auto it = args.rbegin(); it != args.rend(); ++it)
				it->push(scope);
			expression(nil).push(scope);
			callee.push(scope);
			scope.emit(bc::CALL, result, bc::reg(args.size()));
			scope.discard_regs(result + 1);
			return expression(result);
		}

		// Strict `{}` initializes the declared storage type rather than constructing a dynamic table.
		std::optional<expression> strict_default_initializer(func_scope& scope, const strict::typespec& type) {
			switch (type.kind) {
				case strict::type_kind::boolean:
					return expression(any(false), type);
				case strict::type_kind::number:
				case strict::type_kind::sized_int:
				case strict::type_kind::f32:
				case strict::type_kind::f64:
					return expression(any(number(0)), type);
				case strict::type_kind::string:
					return expression(any(scope.fn.own(string::create(scope.fn.L, ""))), type);
				case strict::type_kind::optional: {
					expression result(nil, type);
					return result;
				}
				case strict::type_kind::class_ref:
				case strict::type_kind::struct_ref:
					if (type.declared) {
						std::span<const expression> no_arguments;
						auto                        result = emit_ordinary_call(scope, expression(any(type.declared)), no_arguments);
						result.static_type                 = type;
						result.fresh_struct_value          = type.kind == strict::type_kind::struct_ref;
						return result;
					}
					return std::nullopt;
				default:
					return std::nullopt;
			}
		}

		expression copy_struct_value(func_scope& scope, expression value, const strict::typespec& storage_type) {
			if (!scope.fn.strict_mode || storage_type.kind != strict::type_kind::struct_ref || value.fresh_struct_value)
				return value;
			auto* name   = scope.fn.own(string::create(scope.fn.L, "struct_copy"));
			auto  copier = immutable_module_function(scope, "builtin", name);
			if (copier.kind == expr::err)
				return {};
			std::array<expression, 1> arguments = {value};
			auto                      result    = emit_ordinary_call(scope, copier, arguments);
			result.static_type                  = storage_type;
			result.fresh_struct_value           = true;
			return result;
		}

		std::optional<shared::numeric_operation> atomic_numeric_operation(bc::opcode opcode) {
			switch (opcode) {
				case bc::AADD:
					return shared::numeric_operation::add;
				case bc::ASUB:
					return shared::numeric_operation::sub;
				case bc::AMUL:
					return shared::numeric_operation::mul;
				case bc::ADIV:
					return shared::numeric_operation::div;
				case bc::AMOD:
					return shared::numeric_operation::mod;
				default:
					return std::nullopt;
			}
		}

		expression emit_atomic_field_call(func_scope& scope, const expression& target, std::string_view helper_name, std::span<const expression> suffix) {
			auto* name   = scope.fn.own(string::create(scope.fn.L, helper_name));
			auto  helper = immutable_module_function(scope, "builtin", name);
			if (helper.kind == expr::err)
				return {};
			std::array<expression, 4> arguments = {
				 expression(target.idx.table, true),
				 expression(target.idx.field, true),
			};
			LI_ASSERT(suffix.size() <= arguments.size() - 2);
			std::copy(suffix.begin(), suffix.end(), arguments.begin() + 2);
			auto result        = emit_ordinary_call(scope, helper, std::span<const expression>(arguments.data(), suffix.size() + 2));
			result.static_type = target.static_type;
			if (target.struct_element_field)
				scope.emit(bc::TSET, target.writeback_index, target.writeback_value, target.writeback_table);
			return result;
		}

		expression emit_atomic_field_store(func_scope& scope, const expression& target, const expression& value) {
			std::array<expression, 1> suffix = {value};
			return emit_atomic_field_call(scope, target, "atomic_field_store", suffix);
		}

		expression emit_atomic_field_update(func_scope& scope, const expression& target, shared::numeric_operation operation, const expression& operand) {
			std::array<expression, 2> suffix = {
				 expression(any(static_cast<number>(operation))),
				 operand,
			};
			return emit_atomic_field_call(scope, target, "atomic_field_update", suffix);
		}

		enum cleanup_action : uint8_t {
			cleanup_normal,
			cleanup_return,
			cleanup_throw,
			cleanup_break,
			cleanup_continue,
			cleanup_leave,
		};

		void emit_handler_state(func_scope& scope, bc::rel normal, bool cleanup_only, bc::rel cleanup) {
			scope.emit(bc::SETEH, normal, cleanup_only ? 1 : 0, cleanup);
		}

		func_scope* find_cleanup_scope(func_scope* begin, func_scope* end) {
			for (auto* it = begin; it != end; it = it->prev) {
				if (!it->cleanup_entries.empty())
					return it;
			}
			return nullptr;
		}

		bool emit_cleanup_transfer(func_scope& scope, func_scope* begin, func_scope* end, cleanup_action action, bc::reg value = -1) {
			auto* target = find_cleanup_scope(begin, end);
			if (!target)
				return false;
			if (value != -1 && value != target->cleanup_value)
				scope.emit(bc::MOV, target->cleanup_value, value);
			scope.set_reg(target->cleanup_action, any(number(action)));
			scope.emit(bc::JMP, target->cleanup_entries.back());
			return true;
		}

		bool emit_cleanup_helper(func_scope& scope, std::string_view name) {
			auto* helper_name = scope.fn.own(string::create(scope.fn.L, name));
			auto  helper      = immutable_module_function(scope, "coroutine", helper_name);
			if (helper.kind == expr::err)
				return false;
			std::span<const expression> no_args;
			auto                        result = emit_ordinary_call(scope, helper, no_args);
			scope.free_reg(result.reg);
			return true;
		}

		void emit_cleanup_dispatch(func_scope& scope, bc::rel normal_target) {
			scope.set_label_here(scope.cleanup_dispatch);

			auto on_return   = scope.make_label();
			auto on_throw    = scope.make_label();
			auto on_break    = scope.make_label();
			auto on_continue = scope.make_label();
			auto on_leave    = scope.make_label();
			auto test        = scope.alloc_reg();

			auto branch_on = [&](cleanup_action action, bc::rel target) {
				scope.set_reg(test, any(number(action)));
				scope.emit(bc::CEQ, test, scope.cleanup_action, test);
				scope.emit(bc::JS, target, test);
			};
			branch_on(cleanup_return, on_return);
			branch_on(cleanup_throw, on_throw);
			branch_on(cleanup_break, on_break);
			branch_on(cleanup_continue, on_continue);
			branch_on(cleanup_leave, on_leave);
			scope.emit(bc::JMP, normal_target);

			scope.set_label_here(on_return);
			if (!emit_cleanup_transfer(scope, scope.prev, nullptr, cleanup_return, scope.cleanup_value))
				scope.emit(bc::RET, scope.cleanup_value);

			scope.set_label_here(on_throw);
			scope.emit(bc::SETEX, scope.cleanup_value);
			scope.set_reg(test, exception_marker);
			scope.emit(bc::RET, test);

			scope.set_label_here(on_break);
			if (!emit_cleanup_transfer(scope, scope.prev, scope.owner_break, cleanup_break))
				scope.emit(bc::JMP, scope.lbl_break);

			scope.set_label_here(on_continue);
			if (!emit_cleanup_transfer(scope, scope.prev, scope.owner_continue, cleanup_continue))
				scope.emit(bc::JMP, scope.lbl_continue);

			scope.set_label_here(on_leave);
			auto* leave_end = scope.owner_leave ? scope.owner_leave->prev : nullptr;
			if (!emit_cleanup_transfer(scope, scope.prev, leave_end, cleanup_leave))
				scope.emit(bc::JMP, scope.lbl_leave);

			scope.free_reg(test);
		}

		void clear_dead_registers(func_scope& scope, bc::reg first, bc::reg preserve = -1) {
			for (bc::reg reg = first; reg != scope.reg_next; ++reg) {
				if (reg != preserve && !scope.is_local_reg(reg))
					scope.set_reg(reg, nil);
			}
		}

		bool ensure_assignable(func_scope& scope, const expression& target) {
			if (!target.is_lvalue()) {
				scope.lex().error("expected an assignable expression");
				return false;
			}
			if (target.freeze) {
				scope.lex().error("assigning to constant variable");
				return false;
			}
			if (target.kind == expr::env && !scope.fn.mutable_environment) {
				scope.lex().error("implicit environment assignment to '%s' is only valid in the REPL", target.env->c_str());
				return false;
			}
			return true;
		}

		bool is_contextual_step(const lex::token_value& value) { return value.id == lex::token_name && value.str_val->view() == "step"; }

		bool is_typed_array_constructor(std::string_view name) {
			return name == "i8" || name == "u8" || name == "i16" || name == "u16" || name == "i32" || name == "u32" || name == "i64" || name == "u64" ||
					 name == "f32" || name == "f64";
		}

		strict::typespec numeric_literal_type(lex::numeric_kind kind) {
			switch (kind) {
				case lex::numeric_kind::i8:
					return strict::typespec::integer(true, 8);
				case lex::numeric_kind::i16:
					return strict::typespec::integer(true, 16);
				case lex::numeric_kind::i32:
					return strict::typespec::integer(true, 32);
				case lex::numeric_kind::i64:
					return strict::typespec::integer(true, 64);
				case lex::numeric_kind::u8:
					return strict::typespec::integer(false, 8);
				case lex::numeric_kind::u16:
					return strict::typespec::integer(false, 16);
				case lex::numeric_kind::u32:
					return strict::typespec::integer(false, 32);
				case lex::numeric_kind::u64:
					return strict::typespec::integer(false, 64);
				case lex::numeric_kind::f32:
					return strict::typespec::float32();
				case lex::numeric_kind::f64:
					return strict::typespec::float64();
				case lex::numeric_kind::number:
					return strict::typespec::number();
			}
			return strict::typespec::number();
		}
	}

	// Operator traits for parsing.
	//
	static const operator_traits* lookup_operator(lex::state& l, bool binary) {
		std::span<const operator_traits> range;
		if (binary)
			range = {binary_operators};
		else
			range = {unary_operators};

		for (auto& desc : range) {
			if (l.tok.id == desc.token) {
				if (desc.opcode == bc::NOP) {
					l.next();  // Consume, no-op.
					return nullptr;
				} else {
					return &desc;
				}
			}
		}
		return nullptr;
	}

	// Quick throw helper.
	//
	void func_scope::throw_if(const expression& cc, string* msg, bool inv) {
		if (cc.kind == expr::imm && (cc.imm.as_bool() == inv)) {
			return;
		}

		auto tmp = alloc_reg();
		cc.to_reg(*this, tmp);

		auto j = emit(inv ? bc::JS : bc::JNS, 0, tmp);
		set_reg(tmp, msg);
		emit(bc::SETEX, tmp);
		if (lbl_catchpad != 0) {
			emit(bc::JMP, lbl_catchpad);
		} else {
			set_reg(tmp, exception_marker);
			emit(bc::RET, tmp);
		}
		jump_here(j);

		discard_regs(tmp);
	}

	// Writes a function state as a function.
	//
	static function* write_func(func_state& fn, msize_t line, std::optional<bc::reg> implicit_ret = std::nullopt) {
		// Validate IP.
		//
		LI_ASSERT(fn.pc.size() <= BC_MAX_IP);

		// Apply all fixups before writing it.
		//
		for (msize_t ip = 0; ip != fn.pc.size(); ip++) {
			auto& insn = fn.pc[ip];

			auto fix_relative = [&](bc::reg operand) {
				if (!(operand & label_flag))
					return operand;
				bool fixed = false;
				for (auto& [k, v] : fn.label_map) {
					if (operand == k) {
						operand = v - (ip + 1);
						fixed   = true;
						break;
					}
				}
				LI_ASSERT_MSG("unresolved label leftover", fixed);
				return operand;
			};

			const auto& desc = bc::opcode_descs[(uint8_t) insn.o];
			if (desc.a == bc::op_t::rel)
				insn.a = fix_relative(insn.a);
			if (desc.b == bc::op_t::rel)
				insn.b = fix_relative(insn.b);
			if (desc.c == bc::op_t::rel)
				insn.c = fix_relative(insn.c);
		}

		// If routine does not end with a return, add the implicit return.
		//
		if (fn.pc.empty() || fn.pc.back().o != bc::RET) {
			if (!implicit_ret) {
				fn.pc.emplace_back(bc::insn{bc::KIMM, 0});
				fn.pc.back().set_xmm(nil.value);
				implicit_ret = 0;
			}
			fn.pc.push_back({bc::RET, *implicit_ret});
		}

		// Create the function prototype.
		//
		if (!fn.line_table.empty())
			fn.line_table.front().line_delta -= line;
		function_proto* f        = function_proto::create(fn.L, fn.pc, fn.kvalues, fn.line_table);
		f->num_locals            = fn.max_reg_id + 1;
		f->num_uval              = (msize_t) fn.uvalues.size();
		f->return_class_identity = fn.return_class_identity;
		if (fn.attributes) {
			f->attributes = fn.attributes;
			rc::retain(f->attributes);
		}
		if (fn.strict_mode)
			f->signature = std::make_unique<strict_signature>(strict_signature{fn.parameter_types, fn.return_spec});
		string* src_chunk;
		if (fn.decl_name) {
			src_chunk = string::format(fn.L, "'%.*s':%s", (uint32_t) fn.lex.source_name.size(), fn.lex.source_name.data(), fn.decl_name->c_str());
		} else if (line != 0) {
			src_chunk = string::format(fn.L, "'%.*s':lambda-%u", (uint32_t) fn.lex.source_name.size(), fn.lex.source_name.data(), line);
		} else {
			src_chunk = string::format(fn.L, "'%.*s'", (uint32_t) fn.lex.source_name.size(), fn.lex.source_name.data());
		}
		string* previous_src = f->src_chunk;
		f->src_chunk         = src_chunk;
		rc::release(fn.L, previous_src);
		f->src_line = line;

		// Create the function value. The function retains its prototype, so the
		// construction reference is released immediately.
		//
		function* res = function::create(fn.L, f);
		rc::release(fn.L, f);
		if (fn.disable_jit) {
			(void) tier::disable(res);
		}
#if LI_JIT
		else if (fn.L->jit_policy.execution == tier::mode::required) {
			if (auto error = lib::jit_on(fn.L, res, fn.L->jit_policy.verbose)) {
				fn.lex.error("JIT compilation failed for %s (unsupported opcode or backend operation: %s).", res->proto->src_chunk->c_str(), error->c_str());
				rc::release(fn.L, res);
				return nullptr;
			}
		}
#endif
		return res;
	}

	static expression       instantiate_generic(func_scope& scope, generic_template& declaration, std::span<const strict::typespec> explicit_types,
		 std::span<const expression> arguments, const lex::token_value& call_token);
	static strict::typespec function_static_type(function* value);

	static bool is_numeric_type(const strict::typespec& type) {
		return type.kind == strict::type_kind::number || type.kind == strict::type_kind::sized_int || type.kind == strict::type_kind::f32 ||
				 type.kind == strict::type_kind::f64;
	}

	static std::optional<strict::typespec> binary_result_type(func_scope& scope, const expression& lhs, bc::opcode op, const expression& rhs) {
		const auto& left  = lhs.static_type;
		const auto& right = rhs.static_type;
		if (!scope.fn.strict_mode)
			return strict::typespec::any();
		if (op == bc::CEQ || op == bc::CNE) {
			if (left.kind == strict::type_kind::any || right.kind == strict::type_kind::any)
				return strict::typespec::boolean();
			if (!strict::assignable(left, right) && !strict::assignable(right, left)) {
				scope.lex().error("incompatible operand types: _%s_ and _%s_", strict::to_string(left).c_str(), strict::to_string(right).c_str());
				return std::nullopt;
			}
			return strict::typespec::boolean();
		}
		if (left.kind == strict::type_kind::any || right.kind == strict::type_kind::any) {
			scope.lex().error(
				 "strict operator operands must have resolved types, got _%s_ and _%s_", strict::to_string(left).c_str(), strict::to_string(right).c_str());
			return std::nullopt;
		}
		if (op == bc::CLT || op == bc::CGT || op == bc::CLE || op == bc::CGE) {
			if (!((is_numeric_type(left) && is_numeric_type(right)) || (left.kind == strict::type_kind::string && right.kind == strict::type_kind::string))) {
				scope.lex().error("incompatible operand types: _%s_ and _%s_", strict::to_string(left).c_str(), strict::to_string(right).c_str());
				return std::nullopt;
			}
			return strict::typespec::boolean();
		}
		if (op == bc::LAND || op == bc::LOR) {
			if (left.kind != strict::type_kind::boolean || right.kind != strict::type_kind::boolean) {
				scope.lex().error("logical operands must be bool, got _%s_ and _%s_", strict::to_string(left).c_str(), strict::to_string(right).c_str());
				return std::nullopt;
			}
			return strict::typespec::boolean();
		}
		if (op == bc::NCS) {
			if (left.kind == strict::type_kind::optional && !left.children.empty() && strict::assignable(left.children.front(), right))
				return left.children.front();
			if (strict::assignable(left, right) || strict::assignable(right, left))
				return strict::assignable(left, right) ? left : right;
			scope.lex().error("incompatible null-coalescing operands: _%s_ and _%s_", strict::to_string(left).c_str(), strict::to_string(right).c_str());
			return std::nullopt;
		}
		if (op == bc::AADD && left.kind == strict::type_kind::string && right.kind == strict::type_kind::string)
			return strict::typespec::string_type();
		if (!is_numeric_type(left) || !is_numeric_type(right)) {
			scope.lex().error("incompatible operand types: _%s_ and _%s_", strict::to_string(left).c_str(), strict::to_string(right).c_str());
			return std::nullopt;
		}
		if (left.kind == strict::type_kind::number || right.kind == strict::type_kind::number)
			return strict::typespec::number();
		if (left.kind == strict::type_kind::f64 || right.kind == strict::type_kind::f64)
			return strict::typespec::float64();
		if (left.kind == strict::type_kind::f32 || right.kind == strict::type_kind::f32)
			return strict::typespec::float32();
		if (strict::assignable(left, right))
			return left;
		if (strict::assignable(right, left))
			return right;
		scope.lex().error(
			 "integer operands cannot be represented by one result type: _%s_ and _%s_", strict::to_string(left).c_str(), strict::to_string(right).c_str());
		return std::nullopt;
	}

	// Applies an operator to the expressions handling constant folding, returns the resulting expression.
	//
	expression emit_unop(func_scope& scope, bc::opcode op, const expression& rhs) {
		strict::typespec result_type = rhs.static_type;
		if (scope.fn.strict_mode && rhs.static_type.kind == strict::type_kind::any) {
			scope.lex().error("strict unary operand has unresolved type");
			return {};
		}
		if (scope.fn.strict_mode) {
			if (op == bc::LNOT) {
				if (rhs.static_type.kind != strict::type_kind::boolean) {
					scope.lex().error("logical operand must be bool, got _%s_", strict::to_string(rhs.static_type).c_str());
					return {};
				}
				result_type = strict::typespec::boolean();
			} else if (op == bc::ANEG && !is_numeric_type(rhs.static_type)) {
				scope.lex().error("numeric operand required, got _%s_", strict::to_string(rhs.static_type).c_str());
				return {};
			}
		}

		const bool safe_to_fold = rhs.kind == expr::imm && (op == bc::TOBOOL || op == bc::LNOT || (op == bc::ANEG && rhs.imm.is_num()));
		if (safe_to_fold) {
			auto v = apply_unary(scope.fn.L, rhs.imm, op);
			if (!v.is_exc())
				return expression(scope.fn.own(v), result_type);
			scope.fn.L->clear_exception();
		}

		auto r = scope.alloc_reg();
		if (rhs.kind == expr::reg) {
			scope.emit(op, r, rhs.reg);
		} else {
			rhs.to_reg(scope, r);
			scope.emit(op, r, r);
		}
		return expression(r, false, result_type);
	}

	expression emit_binop(func_scope& scope, const expression& lhs, bc::opcode op, const expression& rhs) {
		if (scope.fn.strict_mode && lhs.static_type.kind == strict::type_kind::struct_ref && lhs.static_type.declared) {
			std::optional<trait> which;
			bool                 negate = false;
			if (op == bc::AADD)
				which = trait::add;
			else if (op == bc::CEQ || op == bc::CNE) {
				which  = trait::eq;
				negate = op == bc::CNE;
			} else if (op == bc::CLT)
				which = trait::lt;
			if (which) {
				function* method = nullptr;
				for (vclass* current = lhs.static_type.declared; current && !method; current = current->super) {
					if (current->traits)
						method = current->traits->methods[size_t(*which)];
				}
				if (method) {
					expression                callee(any(method), function_static_type(method));
					std::array<expression, 1> arguments = {rhs};
					if (method->proto && method->proto->generic) {
						callee = instantiate_generic(scope, *method->proto->generic, {}, arguments, scope.lex().last_token);
						if (callee.kind == expr::err)
							return {};
					}
					auto result = scope.alloc_reg(2);
					rhs.push(scope);
					lhs.push(scope);
					callee.push(scope);
					scope.emit(bc::CALL, result, 1);
					scope.discard_regs(result + 1);
					strict::typespec result_type = strict::typespec::any();
					if (callee.static_type.kind == strict::type_kind::function && !callee.static_type.children.empty())
						result_type = callee.static_type.children.back();
					if (result_type.kind == strict::type_kind::self_ref)
						result_type = lhs.static_type;
					expression value(result, false, std::move(result_type));
					if (negate)
						return emit_unop(scope, bc::LNOT, value);
					return value;
				}
			}
		}
		auto result_type = binary_result_type(scope, lhs, op, rhs);
		if (!result_type)
			return {};

		const bool both_immediate = lhs.kind == expr::imm && rhs.kind == expr::imm;
		const bool both_numbers   = both_immediate && lhs.imm.is_num() && rhs.imm.is_num();
		const bool logical_value  = op == bc::NCS || op == bc::LOR || op == bc::LAND;
		const bool safe_equality =
			 (op == bc::CEQ || op == bc::CNE) && both_immediate && ((!lhs.imm.is_gc() && !rhs.imm.is_gc()) || (lhs.imm.is_str() && rhs.imm.is_str()));
		if (both_immediate && (both_numbers || logical_value || safe_equality)) {
			auto v = apply_binary(scope.fn.L, lhs.imm, rhs.imm, op);
			if (!v.is_exc())
				return expression(scope.fn.own(v), *result_type);
			scope.fn.L->clear_exception();
		}

		auto r = scope.alloc_reg();
		if (lhs.kind == expr::reg && rhs.kind == expr::reg) {
			scope.emit(op, r, lhs.reg, rhs.reg);
		} else if (lhs.kind == expr::reg) {
			rhs.to_reg(scope, r);
			scope.emit(op, r, lhs.reg, r);
		} else {
			lhs.to_reg(scope, r);
			scope.emit(op, r, r, rhs.to_anyreg(scope));
		}
		return expression(r, false, *result_type);
	}

	// Parses a primary expression.
	// - If index is set, disables most valid tokens, used to resolve target for operator ::.
	//
	static expression expr_primary(func_scope& scope, bool index = false);
	static expression expr_var(func_scope& scope, string* name);
	static expression expr_parse(func_scope& scope);
	static expression instantiate_generic(func_scope& scope, generic_template& declaration, std::span<const strict::typespec> explicit_types,
		 std::span<const expression> arguments, const lex::token_value& call_token);
	static std::optional<std::vector<strict::typespec>> parse_explicit_type_arguments(func_scope& scope);

	// Handles nested binary and unary operators with priorities, finally calls the expr_simple.
	//
	static const operator_traits* expr_op(func_scope& scope, expression& out, uint8_t prio);

	// Reads the strict tier's source-level type grammar. Nominal names must be
	// compile-time class values; aliases are resolved forward in the parser's
	// lexical type environment.
	//
	static constexpr uint32_t legacy_type_tag  = 0x80000000u;
	static constexpr uint32_t runtime_type_tag = 0x40000000u;

	static strict::typespec legacy_type(value_type value) {
		auto result          = strict::typespec::any();
		result.generic_index = legacy_type_tag | uint32_t(value);
		return result;
	}

	static bool contains_runtime_type(const strict::typespec& value) {
		if (value.kind == strict::type_kind::any && (value.generic_index & runtime_type_tag))
			return true;
		return std::any_of(value.children.begin(), value.children.end(), [](const strict::typespec& child) { return contains_runtime_type(child); });
	}

	static std::optional<strict::typespec> parse_type_name(func_scope& scope) {
		strict::typespec result;
		const bool       contextual_view = scope.lex().tok == lex::token_name && scope.lex().tok.str_val->view() == "view";
		const bool       contextual_weak = scope.lex().tok == lex::token_name && scope.lex().tok.str_val->view() == "weak";
		if (contextual_view) {
			scope.lex().next();
			auto child = parse_type_name(scope);
			if (!child)
				return std::nullopt;
			result = strict::typespec::view_of(std::move(*child));
		} else if (contextual_weak) {
			scope.lex().next();
			const bool child_is_contextual =
				 scope.lex().tok == lex::token_name && (scope.lex().tok.str_val->view() == "view" || scope.lex().tok.str_val->view() == "weak");
			const bool has_child = child_is_contextual || scope.lex().tok == lex::token_number || scope.lex().tok == lex::token_bool ||
										  scope.lex().tok == lex::token_array || scope.lex().tok == lex::token_string || scope.lex().tok == lex::token_function ||
										  scope.lex().tok == lex::token_table || scope.lex().tok == lex::token_nil || scope.lex().tok == lex::token_object ||
										  scope.lex().tok == lex::token_class || scope.lex().tok == lex::token_name;
			if (!has_child && !scope.fn.strict_mode) {
				result = strict::typespec(strict::type_kind::weak_ref);
			} else {
				auto child = parse_type_name(scope);
				if (!child)
					return std::nullopt;
				result = strict::typespec::weak(std::move(*child));
			}
		} else if (scope.lex().opt(lex::token_number)) {
			result = strict::typespec::number();
		} else if (scope.lex().opt(lex::token_bool)) {
			result = strict::typespec::boolean();
		} else if (scope.lex().opt(lex::token_array)) {
			result = strict::typespec::array();
		} else if (scope.lex().opt(lex::token_string)) {
			result = strict::typespec::string_type();
		} else if (scope.lex().opt(lex::token_object)) {
			result = strict::typespec::class_type(nullptr);
		} else if (scope.lex().opt(lex::token_class)) {
			result = legacy_type(type_class);
		} else if (scope.lex().opt(lex::token_function)) {
			result = strict::typespec::function();
		} else if (scope.lex().opt(lex::token_table)) {
			result = strict::typespec::table();
		} else if (scope.lex().opt(lex::token_nil)) {
			result = strict::typespec::nil();
		} else if (scope.lex().tok == lex::token_name) {
			auto name = scope.lex().next();
			auto text = name.str_val->view();
			if (text == "typeof" && scope.lex().opt('(')) {
				auto value = expr_parse(scope);
				if (value.kind == expr::err || scope.lex().check(')') == lex::token_error)
					return std::nullopt;
				result = value.static_type;
			} else if (text == "i8")
				result = strict::typespec::integer(true, 8);
			else if (text == "i16")
				result = strict::typespec::integer(true, 16);
			else if (text == "i32")
				result = strict::typespec::integer(true, 32);
			else if (text == "i64")
				result = strict::typespec::integer(true, 64);
			else if (text == "u8")
				result = strict::typespec::integer(false, 8);
			else if (text == "u16")
				result = strict::typespec::integer(false, 16);
			else if (text == "u32")
				result = strict::typespec::integer(false, 32);
			else if (text == "u64")
				result = strict::typespec::integer(false, 64);
			else if (text == "f32")
				result = strict::typespec::float32();
			else if (text == "f64")
				result = strict::typespec::float64();
			else if (text == "Self" || (scope.fn.class_decl_name && string_value_equals(scope.fn.class_decl_name, name.str_val))) {
				if (!scope.fn.class_decl_identity) {
					scope.lex().error("Self is only valid inside a class or struct declaration");
					return std::nullopt;
				}
				result = strict::typespec::self();
			} else {
				auto alias = range::find_if(scope.fn.type_aliases, [&](const type_alias_entry& entry) { return string_value_equals(entry.name, name.str_val); });
				if (alias != scope.fn.type_aliases.end()) {
					result = alias->value;
				} else {
					auto nominal = expr_var(scope, name.str_val);
					if (nominal.kind != expr::imm || !nominal.imm.is_vcl()) {
						if (scope.fn.strict_mode) {
							scope.lex().error("unresolved type name '%s'", name.str_val->c_str());
							return std::nullopt;
						}
						result.generic_index = runtime_type_tag;
						result.generic_name  = name.str_val;
						if (!scope.fn.type_table || !scope.fn.type_table->set(scope.fn.L, any(name.str_val), nil)) {
							scope.lex().error("failed to reserve runtime type name '%s'", name.str_val->c_str());
							return std::nullopt;
						}
					} else {
						auto* declared = nominal.imm.as_vcl();
						if (declared->generic && scope.lex().tok == '<') {
							auto arguments = parse_explicit_type_arguments(scope);
							if (!arguments)
								return std::nullopt;
							std::span<const expression> no_values;
							auto                        instantiated = instantiate_generic(scope, *declared->generic, *arguments, no_values, name);
							if (instantiated.kind == expr::err || !instantiated.imm.is_vcl())
								return std::nullopt;
							declared = instantiated.imm.as_vcl();
						}
						result = declared->value_semantics ? strict::typespec::struct_type(declared) : strict::typespec::class_type(declared);
					}
				}
			}
		} else {
			scope.lex().error("expected type name, got '%s'", scope.lex().tok.to_string().c_str());
			return std::nullopt;
		}

		while (true) {
			if (scope.lex().opt('?')) {
				result = strict::typespec::optional_of(std::move(result));
				continue;
			}
			if (!scope.lex().opt('['))
				break;
			if (scope.lex().opt(']')) {
				result = strict::typespec::typed_array_of(std::move(result));
				continue;
			}
			auto count = scope.lex().check(lex::token_lnum);
			if (count == lex::token_error)
				return std::nullopt;
			if (count.num_val < 0 || count.num_val != std::trunc(count.num_val) || count.num_val > number(std::numeric_limits<msize_t>::max())) {
				scope.lex().error("fixed array length must be a non-negative integer");
				return std::nullopt;
			}
			if (scope.lex().check(']') == lex::token_error)
				return std::nullopt;
			result = strict::typespec::fixed_array_of(std::move(result), msize_t(count.num_val));
		}
		return result;
	}

	static std::optional<std::vector<strict::typespec>> parse_explicit_type_arguments(func_scope& scope) {
		if (scope.lex().check('<') == lex::token_error)
			return std::nullopt;
		std::vector<strict::typespec> result;
		if (scope.lex().opt('>'))
			return result;
		while (true) {
			string* pack_name = scope.lex().tok == lex::token_name ? scope.lex().tok.str_val : nullptr;
			auto    value     = parse_type_name(scope);
			if (!value)
				return std::nullopt;
			if (scope.lex().opt(lex::token_dots)) {
				auto pack = pack_name ? range::find_if(scope.fn.variadic_type_aliases,
													 [&](const variadic_type_alias_entry& alias) { return string_value_equals(alias.name, pack_name); })
											 : scope.fn.variadic_type_aliases.end();
				if (pack == scope.fn.variadic_type_aliases.end()) {
					scope.lex().error("'%s...' is not a bound variadic generic type pack", pack_name ? pack_name->c_str() : "type");
					return std::nullopt;
				}
				result.insert(result.end(), pack->values.begin(), pack->values.end());
			} else {
				result.push_back(std::move(*value));
			}
			if (scope.lex().opt('>'))
				break;
			if (scope.lex().check(',') == lex::token_error)
				return std::nullopt;
		}
		return result;
	}

	static expression emit_type_test(func_scope& scope, const expression& value, const strict::typespec& expected) {
		if (value.is_type_literal)
			return expression(any(strict::same(value.static_type, expected)), strict::typespec::boolean());
		if (expected.kind == strict::type_kind::optional && !expected.children.empty()) {
			auto result = scope.alloc_reg();
			auto source = value.to_anyreg(scope);
			scope.set_reg(result, nil);
			scope.emit(bc::CEQ, result, source, result);
			auto accepted_nil = scope.emit(bc::JS, 0, result);
			auto child        = emit_type_test(scope, expression(source, true, value.static_type), expected.children.front());
			child.to_reg(scope, result);
			scope.jump_here(accepted_nil);
			scope.discard_regs(result + 1);
			return expression(result, false, strict::typespec::boolean());
		}
		if ((expected.kind == strict::type_kind::view || expected.kind == strict::type_kind::weak_ref) && !expected.children.empty()) {
			if (expected.kind == strict::type_kind::view)
				return emit_type_test(scope, value, expected.children.front());
		}

		auto source = value.to_anyreg(scope);
		auto result = scope.alloc_reg();
		if ((expected.kind == strict::type_kind::class_ref || expected.kind == strict::type_kind::struct_ref) && expected.declared) {
			scope.set_reg(result, any(expected.declared));
			scope.emit(bc::CTYX, result, source, result);
		} else if (expected.kind == strict::type_kind::class_ref) {
			scope.emit(bc::CTY, result, source, type_object);
		} else if (expected.kind == strict::type_kind::self_ref && scope.fn.class_decl_identity) {
			scope.emit(bc::CTYID, result, source);
		} else if (expected.kind == strict::type_kind::any && (expected.generic_index & runtime_type_tag) && expected.generic_name) {
			if (!scope.fn.type_table) {
				scope.lex().error("runtime type registry is unavailable");
				return {};
			}
			auto type_reg = scope.alloc_reg(2);
			scope.set_reg(type_reg, any(scope.fn.type_table));
			scope.set_reg(type_reg + 1, any(expected.generic_name));
			scope.emit(bc::TGETR, type_reg, type_reg + 1, type_reg);
			auto is_class = type_reg + 1;
			scope.emit(bc::CTY, is_class, type_reg, type_class);
			scope.throw_if_not(is_class, "expected '%s' to resolve to a class", expected.generic_name->c_str());
			scope.emit(bc::CTYX, result, source, type_reg);
			scope.free_reg(type_reg, 2);
		} else if (expected.kind == strict::type_kind::any && (expected.generic_index & legacy_type_tag)) {
			scope.emit(bc::CTY, result, source, value_type(expected.generic_index & ~legacy_type_tag));
		} else {
			auto guard = strict::dynamic_guard(expected);
			if (guard == type_invalid) {
				scope.lex().error("type '%s' has no dynamic guard", strict::to_string(expected).c_str());
				return {};
			}
			scope.emit(bc::CTY, result, source, guard);
		}
		return expression(result, false, strict::typespec::boolean());
	}

	static bool guard_value(func_scope& scope, const expression& value, const strict::typespec& expected, std::string_view description) {
		auto check = emit_type_test(scope, value, expected);
		if (check.kind == expr::err)
			return false;
		scope.throw_if_not(check, "expected %.*s to be of type '%s'", int(description.size()), description.data(), strict::to_string(expected).c_str());
		if (check.kind == expr::reg)
			scope.free_reg(check.reg);
		return true;
	}

	// Handles nested binary and unary operators with priorities, finally calls the expr_simple.
	//
	static const operator_traits* expr_op(func_scope& scope, expression& out, uint8_t prio) {
		auto& lhs = out;

		// If token matches a unary operator:
		//
		auto unary_token = scope.lex().tok;
		if (auto op = lookup_operator(scope.lex(), false)) {
			// Parse the next expression, emit unary operator. Outside grouping,
			// a newline before the operand terminates the statement.
			//
			scope.lex().next();
			if (scope.fn.continuation_depth == 0 && scope.lex().tok.source_line > unary_token.end_line) {
				scope.lex().error("newline after unary operator");
				return nullptr;
			}
			expression exp = {};
			expr_op(scope, exp, op->prio_right);
			if (exp.kind == expr::err) {
				out = {};
				return nullptr;
			}

			lhs = emit_unop(scope, op->opcode, exp);
		}
		// Otherwise, parse a primary expression.
		//
		else {
			if (unary_token.id == lex::token_add && scope.fn.continuation_depth == 0 && scope.lex().tok.source_line > unary_token.end_line) {
				scope.lex().error("newline after unary operator");
				return nullptr;
			}
			lhs = expr_primary(scope);
			if (lhs.kind == expr::err) {
				out = {};
				return nullptr;
			}
		}

		// Handle special operators.
		// - IS / AS:
		if (10 < prio && can_continue_expression(scope) && scope.lex().opt(lex::token_is)) {
			auto type = parse_type_name(scope);
			if (!type)
				return nullptr;
			lhs = emit_type_test(scope, lhs, *type);
			if (lhs.kind == expr::err)
				return nullptr;
		}
		if (10 < prio && can_continue_expression(scope) && scope.lex().opt(lex::token_as)) {
			auto type = parse_type_name(scope);
			if (!type)
				return nullptr;
			if (!guard_value(scope, lhs, *type, "converted value"))
				return nullptr;
			lhs.static_type        = std::move(*type);
			lhs.fresh_struct_value = lhs.static_type.kind == strict::type_kind::struct_ref;
		}
		// - IN:
		if (10 < prio && can_continue_expression(scope) && scope.lex().opt(lex::token_in)) {
			expression rhs = expr_primary(scope);
			if (rhs.kind == expr::err) {
				out = {};
				return nullptr;
			}

			auto vin    = any(&lib::detail::builtin_in);
			auto result = scope.alloc_reg();
			lhs.push(scope);
			rhs.push(scope);
			expression(vin).push(scope);
			scope.emit(bc::CALL, result, 1);
			lhs = expression(result, false, strict::typespec::boolean());
		}
		// - Post INC/DEC:
		if (can_continue_expression(scope) && (scope.lex().tok == lex::token_cdec || scope.lex().tok == lex::token_cinc)) {
			// Throw if const.
			//
			if (!ensure_assignable(scope, lhs)) {
				out = {};
				return nullptr;
			}
			auto op = scope.lex().next() == lex::token_cdec ? bc::ASUB : bc::AADD;
			if (lhs.atomic_field) {
				auto operation = atomic_numeric_operation(op);
				LI_ASSERT(operation.has_value());
				auto one     = expression(any(1.0), lhs.static_type);
				auto updated = emit_atomic_field_update(scope, lhs, *operation, one);
				if (updated.kind == expr::err) {
					out = {};
					return nullptr;
				}
				lhs = emit_binop(scope, updated, op == bc::AADD ? bc::ASUB : bc::AADD, one);
			} else {
				auto lhsp    = lhs.to_nextreg(scope);
				auto updated = emit_binop(scope, expression(lhsp), op, expression(any(1.0)));
				lhs.assign(scope, updated);
				scope.discard_regs(lhsp + 1);
				lhs = lhsp;
			}
		}

		// Loop until we exhaust the nested binary operators.
		//
		auto op = can_continue_expression(scope) ? lookup_operator(scope.lex(), true) : nullptr;
		while (op && op->prio_left < prio) {
			scope.lex().next();

			// Logical operators preserve the selected operand and only evaluate
			// the RHS when the LHS does not determine the result.
			//
			if (op->token == lex::token_land || op->token == lex::token_lor) {
				auto result = lhs.to_nextreg(scope);
				auto done   = scope.emit(op->token == lex::token_land ? bc::JNS : bc::JS, 0, result);

				expression rhs    = {};
				auto       nextop = expr_op(scope, rhs, op->prio_right);
				if (rhs.kind == expr::err) {
					out = {};
					return nullptr;
				}
				rhs.to_reg(scope, result);
				scope.jump_here(done);
				scope.discard_regs(result + 1);

				auto result_type = binary_result_type(scope, lhs, op->opcode, rhs);
				if (!result_type) {
					out = {};
					return nullptr;
				}
				lhs = expression(result, false, *result_type);
				op  = nextop;
				continue;
			}

			// Null coalescing is right-associative and must not evaluate its RHS
			// when the LHS is non-nil.
			//
			if (op->token == lex::token_nullc) {
				auto result = lhs.to_nextreg(scope);
				auto is_nil = scope.alloc_reg();
				scope.set_reg(is_nil, nil);
				scope.emit(bc::CEQ, is_nil, result, is_nil);
				auto done = scope.emit(bc::JNS, 0, is_nil);

				expression rhs    = {};
				auto       nextop = expr_op(scope, rhs, op->prio_right + 1);
				if (rhs.kind == expr::err) {
					out = {};
					return nullptr;
				}
				rhs.to_reg(scope, result);
				scope.jump_here(done);
				scope.discard_regs(result + 1);

				auto result_type = binary_result_type(scope, lhs, op->opcode, rhs);
				if (!result_type) {
					out = {};
					return nullptr;
				}
				lhs = expression(result, false, *result_type);
				op  = nextop;
				continue;
			}

			expression rhs    = {};
			auto       nextop = expr_op(scope, rhs, op->prio_right);
			if (rhs.kind == expr::err) {
				out = {};
				return nullptr;
			}

			lhs = emit_binop(scope, lhs, op->opcode, rhs);
			op  = nextop;
		}
		return op;
	}

	static expression expr_parse(func_scope& scope);

	// Parses the next expression. Ternaries have the lowest precedence and
	// recurse through the false branch, making them right-associative.
	//
	static expression expr_parse_core(func_scope& scope) {
		expression condition = {};
		expr_op(scope, condition, UINT8_MAX);
		if (condition.kind == expr::err || !can_continue_expression(scope) || !scope.lex().opt(lex::token_tif)) {
			return condition;
		}
		if (scope.fn.strict_mode && condition.static_type.kind != strict::type_kind::boolean) {
			scope.lex().error("ternary condition must be bool, got '%s'", strict::to_string(condition.static_type).c_str());
			return {};
		}

		auto result      = condition.to_nextreg(scope);
		auto on_false    = scope.emit(bc::JNS, 0, result);
		auto branch_base = scope.reg_next;

		auto on_true = expr_parse(scope);
		if (on_true.kind == expr::err) {
			return {};
		}
		on_true.to_reg(scope, result);
		scope.discard_regs(branch_base);

		if (!can_continue_expression(scope)) {
			scope.lex().error("newline before ternary ':'");
			return {};
		}
		if (scope.lex().check(lex::token_telse) == lex::token_error) {
			return {};
		}
		auto done = scope.emit(bc::JMP);
		scope.jump_here(on_false);

		auto on_false_value = expr_parse(scope);
		if (on_false_value.kind == expr::err) {
			return {};
		}
		on_false_value.to_reg(scope, result);
		scope.discard_regs(branch_base);
		scope.jump_here(done);
		strict::typespec result_type = strict::typespec::any();
		if (scope.fn.strict_mode) {
			if (strict::assignable(on_true.static_type, on_false_value.static_type))
				result_type = on_true.static_type;
			else if (strict::assignable(on_false_value.static_type, on_true.static_type))
				result_type = on_false_value.static_type;
			else {
				scope.lex().error("incompatible ternary branch types '%s' and '%s'", strict::to_string(on_true.static_type).c_str(),
					 strict::to_string(on_false_value.static_type).c_str());
				return {};
			}
		}
		return expression(result, false, std::move(result_type));
	}

	struct parsed_range_suffix {
		expression end{nil};
		expression step{nil};
		bool       inclusive = false;
	};

	static bool parse_range_suffix(func_scope& scope, parsed_range_suffix& suffix, bool block_terminates) {
		const auto range_token = scope.lex().next();
		suffix.inclusive       = range_token == lex::token_rangei;
		const auto tk          = scope.lex().tok.id;
		const bool line_ended  = scope.fn.continuation_depth == 0 && scope.lex().tok.source_line > range_token.end_line;
		const bool open_end    = line_ended || tk == ';' || tk == '}' || tk == ')' || tk == ']' || tk == ',' || tk == lex::token_eof ||
										 is_contextual_step(scope.lex().tok) || (block_terminates && tk == '{');
		if (!open_end) {
			suffix.end = expr_parse_core(scope);
			if (suffix.end.kind == expr::err)
				return false;
		} else if (suffix.inclusive) {
			scope.lex().error("inclusive range requires an end value");
			return false;
		}

		if (!line_ended && is_contextual_step(scope.lex().tok)) {
			scope.lex().next();
			suffix.step = expr_parse_core(scope);
			if (suffix.step.kind == expr::err)
				return false;
		}
		return true;
	}

	static expression emit_range_create(func_scope& scope, const expression& start, const parsed_range_suffix& suffix) {
		auto* create_name = scope.fn.own(string::create(scope.fn.L, "create"));
		auto  create      = immutable_module_function(scope, "range", create_name);
		if (create.kind == expr::err)
			return {};
		std::array<expression, 4> args = {start, suffix.end, suffix.step, expression(any(suffix.inclusive))};
		return emit_ordinary_call(scope, create, args);
	}

	static expression expr_parse(func_scope& scope) {
		auto start = expr_parse_core(scope);
		if (start.kind == expr::err || !can_continue_expression(scope) || (scope.lex().tok != lex::token_range && scope.lex().tok != lex::token_rangei))
			return start;

		parsed_range_suffix suffix;
		if (!parse_range_suffix(scope, suffix, false))
			return {};
		return emit_range_create(scope, start, suffix);
	}

	struct function_shape {
		msize_t parameter_count = 0;
		msize_t required_count  = 0;
		bool    is_vararg       = false;
	};

	// Parses function declaration, returns the function value.
	//
	static expression parse_function(func_scope& scope, string* name = nullptr, function_shape* shape = nullptr, table* attributes = nullptr);

	// Parses a class or value-struct declaration, returns the class value.
	//
	static expression parse_class(
		 func_scope& scope, string* name = nullptr, bool value_semantics = false, table* attributes = nullptr, uint64_t reserved_identity = 0);

	// Parses a format string and returns the result.
	//
	static expression parse_format(func_scope& scope);

	// Parses a call, returns the result.
	//
	static expression parse_call(func_scope& scope, const expression& func, const expression& self, std::span<const strict::typespec> explicit_types = {});

	// Parses block-like constructs, returns the result.
	//
	static expression parse_if(func_scope& scope);
	static expression parse_match(func_scope& scope);
	static expression parse_for(func_scope& scope);
	static expression parse_try(func_scope& scope);
	static expression parse_loop(func_scope& scope);
	static expression parse_while(func_scope& scope);
	static expression parse_lock(func_scope& scope);
	static expression parse_atomic(func_scope& scope);

	// Parses a "statement" expression, which considers both statements and expressions valid.
	// Returns the expression representing the value of the statement.
	//
	static expression expr_stmt(func_scope& scope, bool& fin);

	// Parses a block expression made up of N statements, final one is the expression value
	// unless closed with a semi-colon.
	//
	static expression expr_block(func_scope& scope, bc::reg into = -1, bool no_term = false);

	// Creates a variable expression.
	//
	static expression expr_var(func_scope& scope, string* name) {
		// Handle special names.
		//
		if (name->view() == "self") {
			return expression((int32_t) FRAME_SELF, true, scope.fn.class_decl_identity ? strict::typespec::self() : strict::typespec::any());
		} else if (name->view() == "$F") {
			return expression((int32_t) FRAME_TARGET, true);
		} else if (name->view() == "$E") {
			if (scope.fn.module_table) {
				return expression(any(scope.fn.module_table));
			} else {
				return expression(any(scope.fn.scope_table));
			}
		} else if (name->view() == "$MOD") {
			if (scope.fn.module_name) {
				return expression(any(scope.fn.module_name));
			} else {
				return expression(nil);
			}
		} else if (scope.fn.active_generic && scope.fn.active_generic->declaration_class && string_value_equals(scope.fn.active_generic->name, name)) {
			return expression(any(scope.fn.active_generic->declaration_class), scope.fn.active_generic->declaration_class->value_semantics
																										  ? strict::typespec::struct_type(scope.fn.active_generic->declaration_class)
																										  : strict::typespec::class_type(scope.fn.active_generic->declaration_class));
		} else if (name->view() == "$VA" || (scope.fn.vararg_name && string_value_equals(name, scope.fn.vararg_name))) {
			if (!scope.fn.is_vararg) {
				scope.lex().error("$VA cannot be used in non-vararg function");
				return {};
			}

			// Handle the zero-allocation rest view.
			//
			if (scope.lex().opt(lex::token_ucall)) {
				// ::len()
				auto n = scope.lex().check(lex::token_name);
				if (n == lex::token_error)
					return {};
				if (n.str_val->view() != "len") {
					scope.lex().error("$VA can only be called with len()");
					return {};
				}
				if (scope.lex().check('(') == lex::token_error || scope.lex().check(')') == lex::token_error) {
					return {};
				}

				auto tmp = scope.alloc_reg(2);
				scope.emit(bc::VACNT, tmp);
				scope.set_reg(tmp + 1, number(scope.fn.parameter_count));
				scope.emit(bc::CGE, tmp + 1, tmp, tmp + 1);
				auto has_rest = scope.emit(bc::JS, 0, tmp + 1);
				scope.set_reg(tmp, number(0));
				auto done = scope.emit(bc::JMP);
				scope.jump_here(has_rest);
				scope.set_reg(tmp + 1, number(scope.fn.parameter_count));
				scope.emit(bc::ASUB, tmp, tmp, tmp + 1);
				scope.jump_here(done);
				scope.discard_regs(tmp + 1);
				return expression(tmp);
			}
			// Expect index immediately.
			//
			else {
				if (scope.lex().check('[') == lex::token_error) {
					return {};
				}
				continuation_scope continuation{scope.fn};

				// Parse index, make sure it is an integer.
				//
				auto       tmp = scope.alloc_reg(2);
				expression idx = expr_parse(scope);
				idx.to_reg(scope, tmp + 1);

				if (idx.kind != expr::imm || !idx.imm.is_num()) {
					scope.emit(bc::CTY, tmp, tmp + 1, type_number);
					scope.throw_if_not(tmp, "expected numeric index");
				}

				// Negative indices never alias a fixed argument. Other invalid
				// numeric indices remain invalid after adding the integer offset
				// and VAGET consequently yields nil.
				//
				scope.set_reg(tmp, number(0));
				scope.emit(bc::CLT, tmp, tmp + 1, tmp);
				auto nonnegative = scope.emit(bc::JNS, 0, tmp);
				scope.set_reg(tmp + 1, number(-1));
				auto indexed = scope.emit(bc::JMP);
				scope.jump_here(nonnegative);
				scope.set_reg(tmp, number(scope.fn.parameter_count));
				scope.emit(bc::AADD, tmp + 1, tmp, tmp + 1);
				scope.jump_here(indexed);

				// Skip the ']'.
				//
				if (scope.lex().check(']') == lex::token_error) {
					return {};
				}

				// Read the argument and return.
				//
				scope.emit(bc::VAGET, tmp, tmp + 1);
				scope.discard_regs(tmp + 1);
				return expression(tmp);
			}
		}

		// Try using existing local variable.
		//
		for (auto it = &scope; it; it = it->prev) {
			for (auto lit = it->locals.rbegin(); lit != it->locals.rend(); ++lit) {
				if (string_value_equals(lit->id, name)) {
					if (lit->reg == -1)
						return expression(lit->cxpr, lit->static_type);
					else
						return expression(lit->reg, lit->is_const, lit->static_type);
				}
			}
		}

		// Try finding an argument.
		//
		for (bc::reg n = 0; n != scope.fn.args.size(); n++) {
			if (string_value_equals(scope.fn.args[n].name, name)) {
				return expression(int32_t(-FRAME_SIZE - (n + 1)), false, scope.fn.args[n].static_type);
			}
		}

		// Try self-reference.
		//
		if (scope.fn.decl_name && string_value_equals(name, scope.fn.decl_name)) {
			return expression((int32_t) FRAME_TARGET, true);
		}

		// Try using existing upvalue.
		//
		for (bc::reg i = 0; i != scope.fn.uvalues.size(); i++) {
			if (string_value_equals(scope.fn.uvalues[i].id, name)) {
				return expression(upvalue_t{}, (bc::reg) i, scope.fn.uvalues[i].is_const, scope.fn.uvalues[i].static_type);  // uvalue
			}
		}

		// Try finding a builtin.
		//
		auto* builtin_name = string::create(scope.fn.L, "builtin");
		any   bt           = scope.fn.L->modules->get(scope.fn.L, (any) builtin_name);
		rc::release(scope.fn.L, builtin_name);
		if (bt.is_tbl()) {
			bt = bt.as_tbl()->get(scope.fn.L, (any) name);
			if (bt != nil) {
				return bt;
			}
		}

		// Try borrowing a value by creating an upvalue.
		//
		if (scope.fn.enclosing) {
			expression ex = expr_var(*scope.fn.enclosing, name);
			if (ex.kind != expr::env) {
				if (ex.kind == expr::imm)
					return ex;
				if (scope.fn.strict_mode && ex.static_type.kind == strict::type_kind::view) {
					scope.lex().error("view value '%s' may not be captured", name->c_str());
					return {};
				}
				bool    is_const = ex.freeze != 0;
				bc::reg next_reg = (bc::reg) scope.fn.uvalues.size();
				rc::retain(name);
				scope.fn.uvalues.push_back({name, is_const, next_reg, nil, ex.static_type});
				return expression{upvalue_t{}, next_reg, is_const, ex.static_type};
			}
		}
		return name;  // global
	}

	// Validates a named value against a resolved source-level type.
	//
	static bool type_check_var(func_scope& scope, string* name, const strict::typespec& expected) {
		const auto temporary_begin = scope.reg_next;
		auto       value           = expr_var(scope, name);
		if (value.kind == expr::err)
			return false;
		if (!guard_value(scope, value, expected, util::fmt("variable '%s'", name->c_str())))
			return false;
		scope.discard_regs(temporary_begin);
		return true;
	}

	static bool strict_assignable(func_scope& scope, const strict::typespec& expected, const expression& value, std::string_view subject) {
		if (!value.validate_union_read(scope))
			return false;
		if (!scope.fn.strict_mode)
			return true;
		if (value.static_type.kind == strict::type_kind::any)
			return guard_value(scope, value, expected, subject);
		if (expected.kind == strict::type_kind::self_ref &&
			 (value.static_type.kind == strict::type_kind::class_ref || value.static_type.kind == strict::type_kind::struct_ref) && value.static_type.declared &&
			 value.static_type.declared->identity == scope.fn.class_decl_identity)
			return true;
		if (!strict::assignable(expected, value.static_type)) {
			scope.lex().error("type mismatch for %.*s: expected '%s', got '%s'", int(subject.size()), subject.data(), strict::to_string(expected).c_str(),
				 strict::to_string(value.static_type).c_str());
			return false;
		}
		return true;
	}

	// Applies a function's declared return guard while preserving the value that
	// will be transferred to the caller or through a cleanup chain.
	//
	static expression type_check_return(func_scope& scope, expression value) {
		if (!value.validate_union_read(scope))
			return {};
		if (!scope.fn.has_return_guard) {
			if (scope.fn.strict_mode) {
				if (value.static_type.kind == strict::type_kind::any) {
					scope.lex().error("strict return value has an unresolved type");
					return {};
				}
				if (!scope.fn.inferred_return) {
					scope.fn.inferred_return = value.static_type;
				} else if (!strict::assignable(*scope.fn.inferred_return, value.static_type) && !strict::assignable(value.static_type, *scope.fn.inferred_return)) {
					scope.lex().error("incompatible return types '%s' and '%s'", strict::to_string(*scope.fn.inferred_return).c_str(),
						 strict::to_string(value.static_type).c_str());
					return {};
				}
			}
			return value;
		}
		if (!strict_assignable(scope, scope.fn.return_spec, value, "return value"))
			return {};
		if (contains_runtime_type(scope.fn.return_spec)) {
			const auto result = value.to_anyreg(scope);
			if (!guard_value(scope, expression(result, false, value.static_type), scope.fn.return_spec, "return value"))
				return {};
			return expression(result, false, scope.fn.return_spec);
		}

		const auto result = value.to_anyreg(scope);
		auto       check  = scope.alloc_reg();

		std::string_view expected;
		if (scope.fn.return_class_identity) {
			scope.emit(bc::CTYID, check, result);
			expected = scope.fn.class_decl_name ? scope.fn.class_decl_name->view() : std::string_view{"Self"};
		} else if (scope.fn.return_class) {
			scope.set_reg(check, any(scope.fn.return_class));
			scope.emit(bc::CTYX, check, result, check);
			expected = scope.fn.return_class->name->view();
		} else {
			scope.emit(bc::CTY, check, result, scope.fn.return_type);
			expected = type_names[scope.fn.return_type];
		}

		if (scope.fn.return_nullable) {
			auto is_nil = scope.alloc_reg();
			scope.set_reg(is_nil, nil);
			scope.emit(bc::CEQ, is_nil, result, is_nil);
			scope.emit(bc::LOR, check, check, is_nil);
			scope.free_reg(is_nil);
		}

		scope.throw_if_not(check, "expected return value to be of type '%.*s'", expected.size(), expected.data());
		scope.free_reg(check);
		return expression(result);
	}

	static strict::typespec function_static_type(function* value) {
		if (!value || !value->proto || !value->proto->signature)
			return strict::typespec::function();
		auto signature = value->proto->signature->parameters;
		signature.push_back(value->proto->signature->return_type);
		return strict::typespec::function(std::move(signature));
	}

	static bool indexed_atomic_field(func_scope& scope, const expression& obj, const expression& key) {
		if (!scope.fn.strict_mode || key.kind != expr::imm || !key.imm.is_str())
			return false;
		const strict::typespec* object_type = &obj.static_type;
		if (object_type->kind == strict::type_kind::view && !object_type->children.empty())
			object_type = &object_type->children.front();
		string* name = key.imm.as_str();
		if (object_type->kind == strict::type_kind::self_ref && scope.fn.class_field_types) {
			auto field = range::find_if(*scope.fn.class_field_types, [&](const strict_field_binding& item) { return string_value_equals(item.name, name); });
			if (field != scope.fn.class_field_types->end())
				return field->is_atomic;
		}
		if ((object_type->kind != strict::type_kind::class_ref && object_type->kind != strict::type_kind::struct_ref) || !object_type->declared)
			return false;
		auto nominal = range::find_if(scope.fn.nominal_types, [&](const nominal_type_binding& item) {
			return item.declared == object_type->declared ||
					 (item.declared && object_type->declared && item.declared->identity == object_type->declared->identity);
		});
		if (nominal != scope.fn.nominal_types.end()) {
			auto field = range::find_if(nominal->fields, [&](const strict_field_binding& item) { return string_value_equals(item.name, name); });
			if (field != nominal->fields.end())
				return field->is_atomic;
		}
		for (vclass* current = object_type->declared; current; current = current->super) {
			auto field = range::find_if(current->fields(), [&](const field_pair& item) { return string_value_equals(item.key, name); });
			if (field != current->fields().end())
				return !field->value.is_static && field->value.is_atomic;
		}
		return false;
	}

	static strict::typespec indexed_static_type(func_scope& scope, const expression& obj, const expression& key) {
		const strict::typespec* object_type = &obj.static_type;
		if (object_type->kind == strict::type_kind::view && !object_type->children.empty())
			object_type = &object_type->children.front();
		if ((object_type->kind == strict::type_kind::typed_array || object_type->kind == strict::type_kind::fixed_array) && !object_type->children.empty())
			return object_type->children.front();
		if (key.kind != expr::imm || !key.imm.is_str())
			return strict::typespec::any();
		string* name = key.imm.as_str();
		if (object_type->kind == strict::type_kind::self_ref && scope.fn.class_field_types) {
			auto field = range::find_if(*scope.fn.class_field_types, [&](const strict_field_binding& item) { return string_value_equals(item.name, name); });
			if (field != scope.fn.class_field_types->end())
				return field->value;
		}
		if ((object_type->kind == strict::type_kind::class_ref || object_type->kind == strict::type_kind::struct_ref) && object_type->declared) {
			auto nominal = range::find_if(scope.fn.nominal_types, [&](const nominal_type_binding& item) {
				return item.declared == object_type->declared ||
						 (item.declared && object_type->declared && item.declared->identity == object_type->declared->identity);
			});
			if (nominal != scope.fn.nominal_types.end()) {
				auto field = range::find_if(nominal->fields, [&](const strict_field_binding& item) { return string_value_equals(item.name, name); });
				if (field != nominal->fields.end())
					return field->value;
			}
			for (vclass* current = object_type->declared; current; current = current->super) {
				auto property = range::find_if(current->properties,
					 [&](const property_definition& item) { return item.access == property_access::get && string_value_equals(item.name, name); });
				if (property != current->properties.end() && property->method && property->method->proto && property->method->proto->signature)
					return property->method->proto->signature->return_type;
				auto field = range::find_if(current->fields(), [&](const field_pair& item) { return string_value_equals(item.key, name); });
				if (field == current->fields().end())
					continue;
				if (field->value.is_static && field->value.ty == type::fn) {
					auto stored = any::load_from(current->static_space() + field->value.offset, type::fn);
					return stored.is_fn() ? function_static_type(stored.as_fn()) : strict::typespec::function();
				}
				return strict::parse_from_dynamic(to_value_type(field->value.ty));
			}
		}
		return strict::typespec::any();
	}

	// Creates an index expression.
	//
	static expression expr_index(func_scope& scope, const expression& obj, const expression& key, bool handle_null = false) {
		if (obj.kind == expr::imm && key.kind == expr::imm) {
			if (obj.imm.is_tbl()) {
				if (obj.imm.as_tbl()->is_frozen) {
					return (any) obj.imm.as_tbl()->get(scope.fn.L, key.imm);
				}
			}
		}
		auto result_type = indexed_static_type(scope, obj, key);
		if (!handle_null) {
			expression result{obj.to_anyreg(scope), key.to_anyreg(scope), std::move(result_type)};
			result.atomic_field                         = indexed_atomic_field(scope, obj, key);
			const strict::typespec* indexed_object_type = &obj.static_type;
			if (indexed_object_type->kind == strict::type_kind::view && !indexed_object_type->children.empty())
				indexed_object_type = &indexed_object_type->children.front();
			result.fresh_struct_value =
				 result.static_type.kind == strict::type_kind::struct_ref &&
				 (indexed_object_type->kind == strict::type_kind::typed_array || indexed_object_type->kind == strict::type_kind::fixed_array);
			if (obj.kind == expr::idx && obj.static_type.kind == strict::type_kind::struct_ref) {
				result.struct_element_field = true;
				result.writeback_table      = obj.idx.table;
				result.writeback_index      = obj.idx.field;
				result.writeback_value      = result.idx.table;
			}
			if (key.kind == expr::imm && key.imm.is_str()) {
				const strict::typespec* object_type = &obj.static_type;
				if (object_type->kind == strict::type_kind::view && !object_type->children.empty())
					object_type = &object_type->children.front();
				vclass* declared =
					 (object_type->kind == strict::type_kind::class_ref || object_type->kind == strict::type_kind::struct_ref) ? object_type->declared : nullptr;
				if (declared) {
					auto nominal = range::find_if(scope.fn.nominal_types, [&](const nominal_type_binding& item) {
						return item.declared == declared || (item.declared && declared && item.declared->identity == declared->identity);
					});
					if (nominal != scope.fn.nominal_types.end()) {
						auto binding =
							 range::find_if(nominal->fields, [&](const strict_field_binding& item) { return string_value_equals(item.name, key.imm.as_str()); });
						if (binding != nominal->fields.end() && binding->union_group >= 0) {
							result.union_owner   = declared;
							result.union_field   = binding->name;
							result.union_group   = binding->union_group;
							result.union_read_at = scope.lex().last_token;
						}
					}
					for (vclass* current = declared; current; current = current->super) {
						auto field = range::find_if(current->fields(), [&](const field_pair& item) {
							return item.value.is_static && item.value.ty == type::fn && string_value_equals(item.key, key.imm.as_str());
						});
						if (field != current->fields().end()) {
							auto method = any::load_from(current->static_space() + field->value.offset, type::fn);
							if (method.is_fn() && method.as_fn()->proto && method.as_fn()->proto->generic)
								result.generic_owner = method.as_fn()->proto->generic.get();
							break;
						}
					}
				}
			}
			return result;
		}

		if (obj.kind == expr::imm && obj.imm == nil) {
			return nil;
		}

		auto res = scope.alloc_reg();
		auto cc  = scope.alloc_reg();
		auto o   = obj.to_anyreg(scope);
		auto k   = key.to_anyreg(scope);

		scope.set_reg(res, nil);
		scope.emit(bc::CEQ, cc, o, res);
		auto j = scope.emit(bc::JS, 0, cc);
		scope.emit(bc::TGET, res, k, o);
		scope.jump_here(j);
		scope.discard_regs(res + 1);
		return expression{res, true, strict::typespec::optional_of(std::move(result_type))};
	}

	// Parses an array literal.
	//
	static expression expr_array(func_scope& scope) {
		continuation_scope continuation{scope.fn};
		// TODO: Duplicate template.

		// Create a new array.
		//
		expression result = scope.alloc_reg();
		auto       allocp = scope.emit(bc::ANEW, result.reg);

		// Until list is exhausted push expressions.
		//
		msize_t nexpr = 0;
		if (!scope.lex().opt(']')) {
			while (true) {
				reg_sweeper _r{scope};

				expression value = expr_parse(scope);
				if (value.kind == expr::err) {
					return {};
				}

				expression id{any(number(nexpr++))};
				scope.emit(bc::TSETR, id.to_anyreg(scope), value.to_anyreg(scope), result.reg);

				if (scope.lex().opt(']'))
					break;
				else {
					if (scope.lex().check(',') == lex::token_error) {
						return {};
					}
				}
			}
		}
		scope.fn.pc[allocp].b = nexpr;
		result.static_type    = strict::typespec::array();
		return result;
	}

	// Parses a table literal.
	//
	static expression expr_table(func_scope& scope) {
		continuation_scope continuation{scope.fn};
		// TODO: Duplicate template.

		// Create a new table.
		//
		expression result = scope.alloc_reg();
		auto       allocp = scope.emit(bc::TNEW, result.reg);

		// Until list is exhausted set fields.
		//
		msize_t nexpr = 0;
		if (!scope.lex().opt('}')) {
			while (true) {
				reg_sweeper _r{scope};

				auto field = scope.lex().check(lex::token_name);
				if (field == lex::token_error) {
					return {};
				}

				expression value;
				if (scope.lex().opt(':')) {
					value = expr_parse(scope);
				} else {
					value = expr_var(scope, field.str_val);
				}
				if (value.kind == expr::err) {
					return {};
				}
				value = value.to_anyreg(scope);

				auto tmp = scope.alloc_reg();
				scope.set_reg(tmp, any(field.str_val));
				scope.emit(bc::TSETR, tmp, value.reg, result.reg);
				++nexpr;

				if (scope.lex().opt('}'))
					break;
				else {
					if (scope.lex().check(',') == lex::token_error) {
						return {};
					}
				}
			}
		}
		scope.fn.pc[allocp].b = nexpr;
		result.static_type    = strict::typespec::table();
		return result;
	}

	// Solving ambigious syntax with block vs table.
	//
	static bool is_table_init(func_scope& scope) {
		auto& lex = scope.lex();
		if (auto lh = lex.lookahead(); lh == '}') {
			return !scope.first_scope;
		} else if (lh != lex::token_name) {
			return false;
		}

		// Yes we really need double look-ahead.
		//
		auto pi   = lex.input;
		auto pl   = lex.line;
		auto ll   = lex.scan();
		lex.input = pi;
		lex.line  = pl;
		return (ll.id == ':' || ll.id == ',');
	}

	// Parses a primary expression.
	// - If index is set, disables most valid tokens, used to resolve target for operator ::.
	//
	static expression expr_primary(func_scope& scope, bool index) {
		expression base = {};

		// Handle pre inc/dec specially.
		//
		if (scope.lex().tok == lex::token_cdec || scope.lex().tok == lex::token_cinc) {
			auto prefix = scope.lex().next();
			if (scope.fn.continuation_depth == 0 && scope.lex().tok.source_line > prefix.end_line) {
				scope.lex().error("newline after prefix operator");
				return {};
			}
			auto op = prefix == lex::token_cdec ? bc::ASUB : bc::AADD;
			base    = expr_primary(scope);
			if (base.kind == expr::err) {
				return {};
			} else if (!ensure_assignable(scope, base)) {
				return {};
			}
			if (base.atomic_field) {
				auto operation = atomic_numeric_operation(op);
				LI_ASSERT(operation.has_value());
				base = emit_atomic_field_update(scope, base, *operation, expression(any(1.0), base.static_type));
			} else {
				auto result = emit_binop(scope, base, op, expression(any(1.0)));
				base.assign(scope, result);
				base = result;
			}
		}
		// Variables, typed-array constructors, and decorators.
		//
		else if (auto& tk = scope.lex().tok; tk.id == lex::token_name) {
			auto name       = scope.lex().next();
			auto text       = name.str_val->view();
			auto type_alias = range::find_if(scope.fn.type_aliases, [&](const type_alias_entry& entry) { return string_value_equals(entry.name, name.str_val); });
			bool has_value_binding = false;
			for (auto* lexical = &scope; lexical && !has_value_binding; lexical = lexical->prev) {
				has_value_binding =
					 range::find_if(lexical->locals, [&](const local_state& local) { return string_value_equals(local.id, name.str_val); }) != lexical->locals.end();
			}
			if (!has_value_binding) {
				has_value_binding = range::find_if(scope.fn.args, [&](const arg_slot& argument) { return string_value_equals(argument.name, name.str_val); }) !=
										  scope.fn.args.end();
			}

			if (!index && scope.fn.strict_mode && text == "typeof" && scope.lex().opt('(')) {
				auto value = expr_parse(scope);
				if (value.kind == expr::err || scope.lex().check(')') == lex::token_error)
					return {};
				base                 = expression(nil, value.static_type);
				base.is_type_literal = true;
			} else if (!index && scope.fn.strict_mode && text == "typeid" && scope.lex().opt('(')) {
				strict::typespec type;
				bool             type_operand = false;
				if (scope.lex().tok == lex::token_name) {
					auto spelling = scope.lex().tok.str_val->view();
					type_operand  = spelling == "i8" || spelling == "i16" || spelling == "i32" || spelling == "i64" || spelling == "u8" || spelling == "u16" ||
										 spelling == "u32" || spelling == "u64" || spelling == "f32" || spelling == "f64" || spelling == "Self" ||
										 range::find_if(scope.fn.type_aliases, [&](const type_alias_entry& alias) {
											return string_value_equals(alias.name, scope.lex().tok.str_val);
										 }) != scope.fn.type_aliases.end();
					if (!type_operand) {
						auto nominal = expr_var(scope, scope.lex().tok.str_val);
						type_operand = nominal.kind == expr::imm && nominal.imm.is_vcl();
					}
				}
				if (type_operand) {
					auto parsed = parse_type_name(scope);
					if (!parsed)
						return {};
					type = std::move(*parsed);
				} else {
					auto value = expr_parse(scope);
					if (value.kind == expr::err)
						return {};
					type = value.static_type;
				}
				if (scope.lex().check(')') == lex::token_error)
					return {};
				base = expression(any(scope.fn.own(string::create(scope.fn.L, strict::to_string(type)))), strict::typespec::string_type());
			} else if (!index && scope.fn.strict_mode && !has_value_binding && type_alias != scope.fn.type_aliases.end()) {
				base                 = expression(nil, type_alias->value);
				base.is_type_literal = true;
			} else if (!index && text.size() > 1 && text.front() == '@') {
				if (!can_continue_expression(scope) || (scope.lex().tok != '|' && scope.lex().tok != lex::token_lor)) {
					scope.lex().error("decorator must be followed by a closure");
					return {};
				}

				auto* decorator_name = scope.fn.own(string::create(scope.fn.L, text.substr(1)));
				auto  decorator      = expr_var(scope, decorator_name);
				auto  closure        = parse_function(scope);
				if (decorator.kind == expr::err || closure.kind == expr::err)
					return {};
				std::array<expression, 1> args = {closure};
				base                           = emit_ordinary_call(scope, decorator, args);
			} else if (!index && text == "atomic" && can_continue_expression(scope) && scope.lex().tok == '{') {
				base = parse_atomic(scope);
			} else if (!index && text == "lock" && can_continue_expression(scope) && scope.lex().tok != '(' && scope.lex().tok != '.' &&
						  scope.lex().tok != lex::token_ucall) {
				base = parse_lock(scope);
			} else if (!index && text == "shared" && can_continue_expression(scope)) {
				bool shared_constructor = scope.lex().tok == '{' || scope.lex().tok == '[';
				if (!shared_constructor && scope.lex().tok == lex::token_name) {
					lex::state probe = scope.fn.lex;
					probe.next();
					if (probe.tok == lex::token_ucall) {
						probe.next();
						shared_constructor = probe.tok == lex::token_name && probe.tok.str_val->view() == "new";
						if (shared_constructor) {
							probe.next();
							shared_constructor = probe.tok == '(';
						}
					}
				}
				if (shared_constructor) {
					expression value;
					if (scope.lex().opt('{')) {
						value = expr_table(scope);
					} else if (scope.lex().opt('[')) {
						value = expr_array(scope);
					} else {
						value = expr_primary(scope);
					}
					if (value.kind == expr::err)
						return {};
					std::array<expression, 1> args = {value};
					base                           = emit_ordinary_call(scope, expression(any(static_cast<function*>(&atomic::detail::make_shared))), args);
				} else {
					base = expr_var(scope, name.str_val);
				}
			} else if (!index && is_typed_array_constructor(text) && can_continue_expression(scope) && scope.lex().tok == '[' && scope.lex().lookahead() == ']') {
				scope.lex().next();
				scope.lex().next();
				base = immutable_module_function(scope, "typed", name.str_val);
			} else {
				base = expr_var(scope, name.str_val);
			}
		}
		// Sub expressions.
		//
		else if (tk.id == '(') {
			scope.lex().next();
			continuation_scope continuation{scope.fn};
			base = expr_parse(scope);
			if (scope.lex().check(')') == lex::token_error)
				return {};
		}
		// -- Anything below this is not valid for index expression.
		else if (index) {
			scope.lex().error("unexpected token %s", tk.to_string().c_str());
			return {};
		}
		// Constructs.
		//
		else if (tk.id == '#' && scope.fn.strict_mode) {
			scope.lex().next();
			auto name = scope.lex().check(lex::token_name);
			if (name == lex::token_error || scope.lex().check(lex::token_dots) == lex::token_error)
				return {};
			auto pack = range::find_if(
				 scope.fn.variadic_type_aliases, [&](const variadic_type_alias_entry& entry) { return string_value_equals(entry.name, name.str_val); });
			if (pack == scope.fn.variadic_type_aliases.end()) {
				scope.lex().error("unresolved generic type pack '%s'", name.str_val->c_str());
				return {};
			}
			base = expression(any(number(pack->values.size())), strict::typespec::integer(false, 32));
		} else if (tk.id == lex::token_yield) {
			if (scope.fn.lexical_lock_depth) {
				scope.lex().error("yield is not valid directly inside lock");
				return {};
			}
			auto  keyword = scope.lex().next();
			auto* name    = scope.fn.own(string::create(scope.fn.L, "yield"));
			auto  callee  = immutable_module_function(scope, "coroutine", name);
			if (callee.kind == expr::err)
				return {};

			expression value{nil};
			if (scope.lex().tok != ';' && scope.lex().tok != '}' && scope.lex().tok != lex::token_eof && scope.lex().tok.source_line <= keyword.end_line) {
				value = expr_parse(scope);
				if (value.kind == expr::err)
					return {};
			}
			std::array<expression, 1> args = {value};
			base                           = emit_ordinary_call(scope, callee, args);
		} else if (tk.id == lex::token_if) {
			scope.lex().next();
			base = parse_if(scope);
		} else if (tk.id == lex::token_match) {
			scope.lex().next();
			base = parse_match(scope);
		} else if (tk.id == lex::token_loop) {
			scope.lex().next();
			base = parse_loop(scope);
		} else if (tk.id == lex::token_while) {
			scope.lex().next();
			base = parse_while(scope);
		} else if (tk.id == lex::token_for) {
			scope.lex().next();
			base = parse_for(scope);
		} else if (tk.id == lex::token_try) {
			scope.lex().next();
			base = parse_try(scope);
		}
		// Literals.
		//
		else if (tk.id == '[') {
			scope.lex().next();
			base = expr_array(scope);
		} else if (tk.id == '{') {
			if (is_table_init(scope)) {
				scope.lex().next();
				base = expr_table(scope);
			} else {
				scope.lex().next();
				base = expr_block(scope);
			}
		} else if (tk.id == lex::token_lnum) {
			auto literal = scope.lex().next();
			base         = expression(any{literal.num_val}, numeric_literal_type(literal.num_kind));
		} else if (tk.id == lex::token_lstr) {
			base = expression(any{scope.lex().next().str_val});
		} else if (tk.id == lex::token_fstr) {
			base = parse_format(scope);
		} else if (tk.id == lex::token_true) {
			scope.lex().next();
			base = expression(const_true);
		} else if (tk.id == lex::token_false) {
			scope.lex().next();
			base = expression(const_false);
		} else if (tk.id == lex::token_nil) {
			scope.lex().next();
			base = expression(nil);
		} else if (tk.id == lex::token_lor || tk.id == '|') {
			base = parse_function(scope);
		} else {
			scope.lex().error("unexpected token %s", tk.to_string().c_str());
			return {};
		}
		if (base.kind == expr::err)
			return {};

		// Parse suffixes.
		//
		while (true) {
			if (!can_continue_expression(scope))
				return base;

			switch (scope.lex().tok.id) {
				case '<': {
					generic_template* declaration = base.generic_owner;
					if (!declaration && base.kind == expr::imm && base.imm.is_fn() && base.imm.as_fn()->proto && base.imm.as_fn()->proto->generic)
						declaration = base.imm.as_fn()->proto->generic.get();
					if (!declaration && base.kind == expr::imm && base.imm.is_vcl() && base.imm.as_vcl()->generic)
						declaration = base.imm.as_vcl()->generic.get();
					if (!declaration)
						return base;
					auto call_token = scope.lex().tok;
					auto types      = parse_explicit_type_arguments(scope);
					if (!types)
						return {};
					const bool class_template = !declaration->candidates.empty() && declaration->candidates.front().class_template;
					if (class_template) {
						std::span<const expression> no_values;
						base = instantiate_generic(scope, *declaration, *types, no_values, call_token);
					} else {
						if (scope.lex().tok != '(' && scope.lex().tok != lex::token_lstr && scope.lex().tok != '{') {
							scope.lex().error_at(call_token, "generic function arguments must be followed by a call");
							return {};
						}
						base = parse_call(scope, base, nil, *types);
					}
					if (base.kind == expr::err)
						return {};
					break;
				}
				// Call.
				case '{': {
					if ((scope.fn.block_terminator_depth && scope.fn.continuation_depth == 0) || !is_table_init(scope))
						return base;
				}
				case lex::token_lstr:
				case '(': {
					if (index) {
						return base;
					}

					base = parse_call(scope, base, nil);
					if (base.kind == expr::err)
						return {};
					break;
				}
				case lex::token_ucall: {
					if (index) {
						scope.lex().error("unexpected token %s", scope.lex().tok.to_string().c_str());
						return {};
					}

					scope.lex().next();
					expression func;
					if (scope.lex().tok == lex::token_name && scope.lex().tok.str_val->view() == "new") {
						scope.lex().next();
						func = any(static_cast<function*>(&lib::detail::builtin_class_new));
					} else {
						func = expr_primary(scope, true);
					}
					if (func.kind == expr::err)
						return {};
					base = parse_call(scope, func, base);
					if (base.kind == expr::err)
						return {};
					break;
				}
				case lex::token_icall: {
					scope.lex().error("'->' is a return annotation, not a member-call operator; use '.name(...)'");
					return {};
				}

				// Index.
				case lex::token_idxif:
				case '[': {
					bool               nullish = scope.lex().next() == lex::token_idxif;
					continuation_scope continuation{scope.fn};
					expression         field = expr_parse(scope);
					if (field.kind == expr::err || scope.lex().check(']') == lex::token_error)
						return {};
					base = expr_index(scope, base, field, nullish);
					break;
				}
				case lex::token_idxlif:
				case '.': {
					bool    nullish = scope.lex().next() == lex::token_idxlif;
					string* field_name;
					if (lex::is_token_keyword(scope.lex().tok.id)) {
						// Keywords are ordinary member names after '.'.
						auto keyword = scope.lex().next();
						field_name   = scope.fn.own(string::create(scope.fn.L, lex::cx_token_to_strv(keyword.id)));
					} else {
						auto ftk = scope.lex().check(lex::token_name);
						if (ftk == lex::token_error)
							return {};
						field_name = ftk.str_val;
					}
					if (scope.fn.strict_mode && scope.lex().tok == '!' && field_name->view() == "to")
						scope.lex().next();
					expression field = any(field_name);
					expression self  = base;
					base             = expr_index(scope, base, field, nullish);
					if (base.kind == expr::err)
						return {};
					auto member_type = base.static_type;
					if (!index && !nullish && base.generic_owner && scope.lex().tok == '<') {
						auto types = parse_explicit_type_arguments(scope);
						if (!types)
							return {};
						base = parse_call(scope, base, self, *types);
						if (base.kind == expr::err)
							return {};
						member_type             = base.static_type;
						base.fresh_struct_value = base.static_type.kind == strict::type_kind::struct_ref;
					} else if (!index && !nullish && can_continue_expression(scope) &&
								  (scope.lex().tok == '(' || scope.lex().tok == lex::token_lstr || (scope.lex().tok == '{' && is_table_init(scope)))) {
						base = parse_call(scope, base, self);
						if (base.kind == expr::err)
							return {};
						if (member_type.kind == strict::type_kind::function && !member_type.children.empty()) {
							auto declared_return = member_type.children.back();
							if (declared_return.kind == strict::type_kind::self_ref)
								declared_return = self.static_type;
							base.static_type = std::move(declared_return);
						}
						base.fresh_struct_value = base.static_type.kind == strict::type_kind::struct_ref;
					}
					break;
				}
				default: {
					return base;
				}
			}
		}
		return base;
	}

	static bool has_attribute(vm* L, table* attributes, std::string_view name) {
		if (!attributes)
			return false;
		auto* key   = string::create(L, name);
		auto  value = attributes->get(L, any(key));
		rc::release(L, key);
		return value.is_arr();
	}

	static std::optional<any> parse_attribute_literal(func_scope& scope) {
		if (scope.lex().opt('-')) {
			auto token = scope.lex().check(lex::token_lnum);
			if (token == lex::token_error)
				return std::nullopt;
			return any(-token.num_val);
		}
		auto token = scope.lex().next();
		switch (token.id) {
			case lex::token_lnum:
				return any(token.num_val);
			case lex::token_lstr:
				return any(token.str_val);
			case lex::token_true:
				return any(true);
			case lex::token_false:
				return any(false);
			case lex::token_nil:
				return any(nil);
			default:
				scope.lex().error("attribute arguments must be literals");
				return std::nullopt;
		}
	}

	static table* parse_attribute_list(func_scope& scope) {
		if (scope.lex().check('[') == lex::token_error || scope.lex().check('[') == lex::token_error)
			return nullptr;
		auto* result = scope.fn.own(table::create(scope.fn.L));
		while (true) {
			auto name = scope.lex().check(lex::token_name);
			if (name == lex::token_error)
				return nullptr;
			if (result->contains(any(name.str_val))) {
				scope.lex().error("duplicate attribute '%s'", name.str_val->c_str());
				return nullptr;
			}
			auto* arguments = scope.fn.own(array::create(scope.fn.L));
			if (scope.lex().opt('(')) {
				if (!scope.lex().opt(')')) {
					while (true) {
						auto value = parse_attribute_literal(scope);
						if (!value || !arguments->push(scope.fn.L, *value))
							return nullptr;
						if (scope.lex().opt(')'))
							break;
						if (scope.lex().check(',') == lex::token_error)
							return nullptr;
					}
				}
			}
			if (!result->set(scope.fn.L, any(name.str_val), any(arguments)))
				return nullptr;
			if (scope.lex().opt(','))
				continue;
			if (scope.lex().check(']') == lex::token_error || scope.lex().check(']') == lex::token_error)
				return nullptr;
			break;
		}
		result->is_frozen = true;
		any frozen        = set_trait(scope.fn.L, any(result), trait::freeze, any(true));
		if (frozen.is_exc()) {
			if (scope.fn.L->last_ex.is_str())
				scope.lex().error("%s", scope.fn.L->last_ex.as_str()->c_str());
			else
				scope.lex().error("failed to freeze attribute metadata");
			return nullptr;
		}
		rc::release(scope.fn.L, frozen);
		any sealed = set_trait(scope.fn.L, any(result), trait::seal, any(true));
		if (sealed.is_exc()) {
			if (scope.fn.L->last_ex.is_str())
				scope.lex().error("%s", scope.fn.L->last_ex.as_str()->c_str());
			else
				scope.lex().error("failed to seal attribute metadata");
			return nullptr;
		}
		rc::release(scope.fn.L, sealed);
		return result;
	}

	static bool function_has_unannotated_parameter(const lex::state& source) {
		lex::state probe = source;
		if (!probe.opt('(') || probe.opt(')'))
			return false;
		while (probe.tok != lex::token_eof) {
			if (probe.opt(lex::token_dots))
				return false;
			if (probe.tok != lex::token_name)
				return false;
			probe.next();
			if (probe.opt(lex::token_dots))
				return false;
			probe.opt('?');
			if (!probe.opt(':'))
				return true;

			int square = 0;
			int angle  = 0;
			while (probe.tok != lex::token_eof) {
				if (probe.tok == '[')
					++square;
				else if (probe.tok == ']')
					--square;
				else if (probe.tok == '<')
					++angle;
				else if (probe.tok == '>')
					--angle;
				else if (square == 0 && angle == 0 && (probe.tok == ',' || probe.tok == ')'))
					break;
				probe.next();
			}
			if (probe.opt(')'))
				return false;
			if (!probe.opt(','))
				return false;
		}
		return false;
	}

	static bool parse_generic_parameter_list(func_scope& scope, std::vector<generic_parameter>& parameters) {
		if (scope.lex().check('<') == lex::token_error)
			return false;
		if (scope.lex().opt('>'))
			return true;
		while (true) {
			auto name = scope.lex().check(lex::token_name);
			if (name == lex::token_error)
				return false;
			generic_parameter parameter;
			parameter.name     = name.str_val;
			parameter.variadic = scope.lex().opt(lex::token_dots).has_value();
			if (scope.lex().opt(':')) {
				parameter.constraint = parse_type_name(scope);
				if (!parameter.constraint)
					return false;
			}
			parameters.push_back(std::move(parameter));
			if (scope.lex().opt('>'))
				break;
			if (parameters.back().variadic) {
				scope.lex().error("variadic generic parameter '%s' must be last", parameters.back().name->c_str());
				return false;
			}
			if (scope.lex().check(',') == lex::token_error)
				return false;
		}
		return true;
	}

	static bool probe_generic_signature(func_scope& scope, generic_candidate& candidate) {
		lex::state saved        = scope.fn.lex;
		scope.fn.lex            = candidate.source;
		const size_t alias_base = scope.fn.type_aliases.size();
		for (size_t index = 0; index != candidate.parameters.size(); ++index) {
			rc::retain(candidate.parameters[index].name);
			scope.fn.type_aliases.push_back({candidate.parameters[index].name, strict::typespec::generic(uint32_t(index), candidate.parameters[index].name)});
		}

		bool ok = scope.lex().check('(') != lex::token_error;
		if (ok && !scope.lex().opt(')')) {
			while (true) {
				if (scope.lex().opt(lex::token_dots)) {
					candidate.variadic_call_pattern = true;
					ok                              = scope.lex().check(')') != lex::token_error;
					break;
				}
				auto argument = scope.lex().check(lex::token_name);
				if (argument == lex::token_error) {
					ok = false;
					break;
				}
				scope.lex().opt('?');
				strict::typespec pattern;
				if (scope.lex().opt(':')) {
					auto parsed = parse_type_name(scope);
					if (!parsed) {
						ok = false;
						break;
					}
					pattern = std::move(*parsed);
				} else {
					auto existing = range::find_if(
						 candidate.parameters, [&](const generic_parameter& parameter) { return string_value_equals(parameter.name, argument.str_val); });
					size_t index;
					if (existing == candidate.parameters.end()) {
						index = candidate.parameters.size();
						candidate.parameters.push_back({argument.str_val, std::nullopt, false});
					} else {
						index = size_t(existing - candidate.parameters.begin());
					}
					pattern = strict::typespec::generic(uint32_t(index), candidate.parameters[index].name);
				}
				candidate.parameter_patterns.push_back(std::move(pattern));
				if (scope.lex().opt(lex::token_dots))
					candidate.variadic_call_pattern = true;
				if (scope.lex().opt(')'))
					break;
				if (scope.lex().check(',') == lex::token_error) {
					ok = false;
					break;
				}
			}
		}

		while (scope.fn.type_aliases.size() != alias_base) {
			rc::release(scope.fn.L, scope.fn.type_aliases.back().name);
			scope.fn.type_aliases.pop_back();
		}
		scope.fn.lex = std::move(saved);
		return ok;
	}

	static bool skip_generic_declaration(func_scope& scope) {
		bool seen_body = false;
		int  depth     = 0;
		while (scope.lex().tok != lex::token_eof) {
			if (scope.lex().tok == '{') {
				seen_body = true;
				++depth;
			} else if (scope.lex().tok == '}' && seen_body) {
				--depth;
				scope.lex().next();
				if (depth == 0)
					return true;
				continue;
			}
			scope.lex().next();
		}
		scope.lex().error("unterminated generic declaration");
		return false;
	}

	static expression make_generic_placeholder(
		 func_scope& scope, string* name, bool class_template, bool value_semantics, const std::shared_ptr<generic_template>& declaration) {
		if (class_template) {
			auto* placeholder    = scope.fn.own(vclass::create(scope.fn.L, name, {}, {}, {}, nullptr, reserve_class_identity(scope.fn.L), value_semantics));
			placeholder->generic = declaration;
			declaration->declaration_class = placeholder;
			if (!declaration->candidates.empty() && declaration->candidates.front().attributes) {
				placeholder->attributes = declaration->candidates.front().attributes;
				rc::retain(placeholder->attributes);
			}
			return expression(any(placeholder), value_semantics ? strict::typespec::struct_type(placeholder) : strict::typespec::class_type(placeholder));
		}

		func_state placeholder_state{scope.fn, scope};
		placeholder_state.strict_mode = true;
		placeholder_state.set_decl_name(name);
		if (!declaration->candidates.empty())
			placeholder_state.set_attributes(declaration->candidates.front().attributes);
		function* placeholder = write_func(placeholder_state, scope.lex().line);
		if (!placeholder)
			return {};
		scope.fn.own(placeholder);
		placeholder->proto->generic    = declaration;
		declaration->declaration_value = placeholder;
		expression result(any(placeholder), strict::typespec::function());
		result.generic_owner = declaration.get();
		return result;
	}

	static expression parse_generic_declaration(func_scope& scope, string* name, bool class_template, bool value_semantics, table* attributes,
		 bool has_explicit_parameters, function* existing_function = nullptr) {
		struct preserve_strict_mode {
			func_state& fn;
			bool        strict;
			bool        class_strict;
			~preserve_strict_mode() {
				fn.strict_mode       = strict;
				fn.class_strict_mode = class_strict;
			}
		} preserve{scope.fn, scope.fn.strict_mode, scope.fn.class_strict_mode};
		std::vector<generic_parameter> parameters;
		if (has_explicit_parameters && !parse_generic_parameter_list(scope, parameters))
			return {};
		generic_candidate candidate(scope.fn.lex);
		candidate.parameters          = std::move(parameters);
		candidate.attributes          = attributes;
		candidate.class_template      = class_template;
		candidate.value_semantics     = value_semantics;
		candidate.class_decl_name     = scope.fn.class_decl_name;
		candidate.class_decl_identity = scope.fn.class_decl_identity;
		if (attributes)
			rc::retain(attributes);
		if (!class_template && !probe_generic_signature(scope, candidate)) {
			rc::release(scope.fn.L, candidate.attributes);
			candidate.attributes = nullptr;
			return {};
		}

		std::shared_ptr<generic_template> declaration = existing_function && existing_function->proto ? existing_function->proto->generic : nullptr;
		auto existing = existing_function ? expression(any(existing_function), function_static_type(existing_function)) : expr_var(scope, name);
		if (class_template && existing.kind == expr::imm && existing.imm.is_vcl())
			declaration = existing.imm.as_vcl()->generic;
		else if (!class_template && existing.kind == expr::imm && existing.imm.is_fn() && existing.imm.as_fn()->proto)
			declaration = existing.imm.as_fn()->proto->generic;
		if (!declaration)
			declaration = std::make_shared<generic_template>(scope.fn.L, name);
		declaration->candidates.push_back(std::move(candidate));
		if (!skip_generic_declaration(scope))
			return {};
		if (existing.kind == expr::imm &&
			 ((class_template && existing.imm.is_vcl() && existing.imm.as_vcl()->generic == declaration) ||
				  (!class_template && existing.imm.is_fn() && existing.imm.as_fn()->proto && existing.imm.as_fn()->proto->generic == declaration))) {
			existing.generic_owner = declaration.get();
			return existing;
		}
		return make_generic_placeholder(scope, name, class_template, value_semantics, declaration);
	}

	// Parses variable declaration.
	//
	static bool parse_decl(func_scope& scope, bool is_export, table* attributes = nullptr) {
		// Get token traits and skip it.
		//
		auto keyword   = scope.lex().tok;
		bool is_func   = keyword == lex::token_fn;
		bool is_struct = keyword == lex::token_struct;
		bool is_class  = keyword == lex::token_class || is_struct;
		bool is_const  = is_func || is_class || keyword == lex::token_const;
		scope.lex().next();

		// Validate exports.
		//
		if (is_export) {
			if (scope.fn.enclosing) {
				scope.lex().error("export is only valid at top-level declarations.");
				return {};
			}
			if (!is_func && keyword != lex::token_const) {
				scope.lex().error("expected fn or const after export.");
				return {};
			}
		}
		const bool repl_binding = scope.fn.is_repl;
		is_export |= repl_binding;

		// If function or class declaration:
		//
		if (is_func || is_class) {
			auto name = scope.lex().check(lex::token_name);
			if (name == lex::token_error) {
				return false;
			}

			const bool declaration_strict = scope.fn.strict_mode || has_attribute(scope.fn.L, attributes, "strict");
			const bool explicit_generic   = scope.lex().tok == '<';
			const bool auto_generic       = is_func && declaration_strict && scope.lex().tok == '(' && function_has_unannotated_parameter(scope.fn.lex);
			if (explicit_generic && !declaration_strict) {
				scope.lex().error("generic declarations require strict mode");
				return false;
			}
			if (is_func && !explicit_generic && scope.lex().tok != '(') {
				scope.lex().check('(');
				return false;
			}

			auto res = explicit_generic || auto_generic
								? parse_generic_declaration(scope, name.str_val, is_class, is_struct, attributes, explicit_generic)
								: (is_func ? parse_function(scope, name.str_val, nullptr, attributes) : parse_class(scope, name.str_val, is_struct, attributes));
			if (res.kind == expr::err) {
				return {};
			}

			if (repl_binding) {
				expression(name.str_val).assign(scope, res);
				return true;
			}

			expression result;
			if (is_const && res.kind == expr::imm) {
				scope.add_local_cxpr(name.str_val, res.imm, res.static_type);
				result = res;
			} else {
				bc::reg reg = scope.add_local(name.str_val, is_const, res.static_type);
				res.to_reg(scope, reg);
				result = expression(reg, is_const, res.static_type);
			}
			if (is_export)
				expression(export_t{}, name.str_val).assign(scope, result);
			return true;
		}

		// If de-structruing assignment:
		//
		bool is_arr = scope.lex().opt('[').has_value();
		bool is_tbl = scope.lex().opt('{').has_value();
		if (is_arr || is_tbl) {
			if (scope.fn.strict_mode) {
				scope.lex().error("strict destructuring bindings require explicit element types");
				return false;
			}
			// Collect variable list.
			//
			std::pair<string*, any> mappings[32];
			size_t                  size = 0;
			if (is_tbl) {
				while (true) {
					if (size == std::size(mappings)) {
						scope.lex().error("too many variables");
						return false;
					}

					// Valid field?
					//
					string* key;
					string* field;
					if (auto ktk = scope.lex().check(lex::token_name); ktk == lex::token_error) {
						return false;
					} else {
						field = key = ktk.str_val;
					}

					// Remapped?
					//
					if (scope.lex().opt(':')) {
						if (auto ktk = scope.lex().check(lex::token_name); ktk == lex::token_error) {
							return false;
						} else {
							key = ktk.str_val;
						}
					}

					// Write the entry.
					//
					mappings[size++] = {key, any(field)};

					// End?
					//
					if (scope.lex().opt('}'))
						break;
					else if (scope.lex().check(',') == lex::token_error)
						return false;
				}
			} else if (is_arr) {
				number next_index = 0;
				while (true) {
					if (size == std::size(mappings)) {
						scope.lex().error("too many variables");
						return false;
					}

					// Skipped.
					//
					if (scope.lex().opt(',')) {
						next_index += 1;
						continue;
					}

					// Valid mapping.
					//
					if (auto name = scope.lex().check(lex::token_name); name != lex::token_error) {
						mappings[size++] = {name.str_val, any(next_index)};
						next_index += 1;
					}
					// Error.
					else {
						return false;
					}

					// End?
					//
					if (scope.lex().opt(']'))
						break;
					else if (scope.lex().check(',') == lex::token_error)
						return false;
				}
			}

			// Require assignment.
			//
			if (scope.lex().check('=') == lex::token_error)
				return false;

			// Parse the expression.
			//
			expression ex = expr_parse(scope);
			if (ex.kind == expr::err) {
				return false;
			}
			ex = ex.to_anyreg(scope);

			// If export set globals as well as locals.
			//
			if (repl_binding) {
				for (size_t i = 0; i != size; i++) {
					reg_sweeper _g{scope};
					auto        reg = scope.alloc_reg();
					expression(mappings[i].second).to_reg(scope, reg);
					scope.emit(is_arr ? bc::TGETR : bc::TGET, reg, reg, ex.reg);
					expression(mappings[i].first).assign(scope, expression(reg));
				}
			} else if (is_export) {
				for (size_t i = 0; i != size; i++) {
					auto        reg = scope.add_local(mappings[i].first, is_const);
					reg_sweeper _g{scope};
					expression(mappings[i].second).to_reg(scope, reg);
					scope.emit(is_arr ? bc::TGETR : bc::TGET, reg, reg, ex.reg);
					expression{export_t{}, mappings[i].first}.assign(scope, expression(reg));
				}
			}
			// Otherwise assign all locals.
			//
			else {
				for (size_t i = 0; i != size; i++) {
					auto        reg = scope.add_local(mappings[i].first, is_const);
					reg_sweeper _g{scope};
					expression(mappings[i].second).to_reg(scope, reg);
					scope.emit(bc::TGET, reg, reg, ex.reg);
				}
			}
			return true;
		}

		// Get the variable name.
		//
		auto var = scope.lex().check(lex::token_name);
		if (var == lex::token_error)
			return false;

		// If there is a type annotation parse it.
		//
		std::optional<strict::typespec> declared_type;
		if (scope.lex().opt(':')) {
			declared_type = parse_type_name(scope);
			if (!declared_type)
				return false;
		}

		// If immediately assigned:
		//
		if (can_continue_expression(scope) && scope.lex().opt('=')) {
			// Parse the expression.
			//
			expression ex = expr_parse(scope);
			if (ex.kind == expr::err) {
				return false;
			}

			strict::typespec binding_type = declared_type ? *declared_type : ex.static_type;
			if (scope.fn.strict_mode && binding_type.kind == strict::type_kind::any) {
				scope.lex().error("strict binding '%s' has an unresolved type; add an annotation or explicit conversion", var.str_val->c_str());
				return false;
			}
			if (declared_type && !strict_assignable(scope, *declared_type, ex, util::fmt("binding '%s'", var.str_val->c_str())))
				return false;
			ex = copy_struct_value(scope, ex, binding_type);
			if (ex.kind == expr::err)
				return false;

			// REPL declarations live only in the persistent mutable environment;
			// file declarations remain lexical slots (and may be const).
			//
			expression result;
			if (repl_binding) {
				expression(var.str_val).assign(scope, ex);
				result = expression(var.str_val);
			} else if (ex.kind == expr::imm && is_const && ex.imm != nil) {
				scope.add_local_cxpr(var.str_val, ex.imm, binding_type);
				result = expression(ex.imm, binding_type);
			} else {
				auto reg = scope.add_local(var.str_val, is_const, binding_type);
				ex.to_reg(scope, reg);
				result = expression(reg, is_const, binding_type);
			}

			if (declared_type && !scope.fn.strict_mode && !type_check_var(scope, var.str_val, *declared_type))
				return false;

			// If export set global as well.
			//
			if (is_export && !repl_binding)
				expression(export_t{}, var.str_val).assign(scope, result);
		}
		// Otherwise:
		//
		else {
			// Error if const.
			//
			if (is_const) {
				scope.lex().error("const '%s' declared with no initial value.", var.str_val->c_str());
				return false;
			}

			// Error if type checked or strict.
			//
			if (declared_type || scope.fn.strict_mode) {
				scope.lex().error("'%s' requires an initial value.", var.str_val->c_str());
				return false;
			}

			// Uninitialized REPL lets still create/reset their environment slot.
			//
			if (repl_binding) {
				expression(var.str_val).assign(scope, expression(nil));
				return true;
			}

			// Push a new local and load nil.
			//
			auto reg = scope.add_local(var.str_val, is_const);
			scope.set_reg(reg, nil);
		}
		return true;
	}

	// Parses an optionally assign/compound expression.
	//
	static expression parse_assign_or_expr(func_scope& scope) {
		auto expr = expr_parse(scope);
		if (expr.is_lvalue()) {
			// If simple assignment.
			//
			if (can_continue_expression(scope) && scope.lex().opt('=')) {
				// Throw if const.
				//
				if (!ensure_assignable(scope, expr))
					return {};

				// Parse RHS, propagate errors.
				//
				expression value = expr_parse(scope);
				if (value.kind == expr::err)
					return {};

				if (!strict_assignable(scope, expr.static_type, value, "assignment"))
					return {};
				value = copy_struct_value(scope, value, expr.static_type);
				if (value.kind == expr::err)
					return {};

				// Assign the value and preserve the value of the assignment
				// without reading a member target again.
				//
				if (!value.is_value())
					value = expression(value.to_nextreg(scope), false, value.static_type);
				if (expr.atomic_field) {
					auto stored = emit_atomic_field_store(scope, expr, value);
					if (stored.kind == expr::err)
						return {};
					scope.free_reg(stored.reg);
				} else {
					expr.assign(scope, value);
				}
				expr = value;
			} else if (can_continue_expression(scope) && scope.lex().opt(lex::token_cnullc)) {
				// Null-coalescing assignment is lazy and right-associative.
				//
				if (!ensure_assignable(scope, expr))
					return {};

				auto current = expr.to_nextreg(scope);
				auto is_nil  = scope.alloc_reg();
				scope.set_reg(is_nil, nil);
				scope.emit(bc::CEQ, is_nil, current, is_nil);
				auto done = scope.emit(bc::JNS, 0, is_nil);

				auto value = parse_assign_or_expr(scope);
				if (value.kind == expr::err) {
					return {};
				}
				value.to_reg(scope, current);
				if (expr.atomic_field) {
					auto stored = emit_atomic_field_store(scope, expr, expression(current, false, expr.static_type));
					if (stored.kind == expr::err)
						return {};
					scope.free_reg(stored.reg);
				} else {
					expr.assign(scope, expression(current));
				}
				scope.jump_here(done);
				scope.discard_regs(current + 1);
				expr = expression(current);
			} else {
				// Try finding a compound token matching.
				//
				for (auto& binop : binary_operators) {
					if (can_continue_expression(scope) && binop.compound_token && scope.lex().opt(*binop.compound_token)) {
						// Throw if const.
						//
						if (!ensure_assignable(scope, expr))
							return {};

						// Parse RHS, propagate errors.
						//
						expression value = expr_parse(scope);
						if (value.kind == expr::err)
							return value;

						// Emit the operator once, assign it, and keep that result
						// as the value of the compound expression.
						//
						if (expr.atomic_field) {
							auto operation = atomic_numeric_operation(binop.opcode);
							if (!operation) {
								scope.lex().error("atomic fields do not support this compound operator");
								return {};
							}
							if (!binary_result_type(scope, expr, binop.opcode, value))
								return {};
							expr = emit_atomic_field_update(scope, expr, *operation, value);
						} else {
							auto result = emit_binop(scope, expr, binop.opcode, value);
							expr.assign(scope, result);
							if (expr.struct_element_field)
								scope.emit(bc::TSET, expr.writeback_index, expr.writeback_value, expr.writeback_table);
							expr = result;
						}
						break;
					}
				}
			}
		}
		return expr;
	}

	template<typename EmitBody>
	static bool install_defer(func_scope& scope, EmitBody&& emit_body) {
		if (scope.cleanup_entries.empty()) {
			scope.cleanup_action   = scope.alloc_reg();
			scope.cleanup_value    = scope.alloc_reg();
			scope.cleanup_dispatch = scope.make_label();
		}

		const auto outer_normal       = scope.lbl_catchpad;
		const auto outer_cleanup      = scope.lbl_cleanup;
		const bool outer_cleanup_only = scope.catchpad_cleanup;
		const auto previous_entry     = scope.cleanup_entries.empty() ? scope.cleanup_dispatch : scope.cleanup_entries.back();
		const auto handler            = scope.make_label();
		const auto entry              = scope.make_label();
		const auto leave_error        = scope.make_label();
		const auto skip               = scope.emit(bc::JMP);

		scope.set_label_here(handler);
		scope.emit(bc::GETEX, scope.cleanup_value);
		scope.set_reg(scope.cleanup_action, any(number(cleanup_throw)));
		scope.emit(bc::JMP, entry);

		scope.set_label_here(leave_error);
		emit_handler_state(scope, outer_normal, outer_cleanup_only, outer_cleanup);
		if (!emit_cleanup_helper(scope, "@leave_cleanup"))
			return false;
		scope.set_reg(scope.cleanup_value, exception_marker);
		scope.emit(bc::RET, scope.cleanup_value);

		scope.set_label_here(entry);
		emit_handler_state(scope, outer_normal, outer_cleanup_only, outer_cleanup);
		if (!emit_cleanup_helper(scope, "@enter_cleanup"))
			return false;
		emit_handler_state(scope, leave_error, true, outer_cleanup);

		const auto saved_normal       = scope.lbl_catchpad;
		const auto saved_cleanup      = scope.lbl_cleanup;
		const bool saved_cleanup_only = scope.catchpad_cleanup;
		scope.lbl_catchpad            = leave_error;
		scope.lbl_cleanup             = outer_cleanup;
		scope.catchpad_cleanup        = true;
		++scope.fn.defer_body_depth;
		const bool body_ok = emit_body(scope);
		--scope.fn.defer_body_depth;
		scope.lbl_catchpad     = saved_normal;
		scope.lbl_cleanup      = saved_cleanup;
		scope.catchpad_cleanup = saved_cleanup_only;
		if (!body_ok)
			return false;

		emit_handler_state(scope, outer_normal, outer_cleanup_only, outer_cleanup);
		if (!emit_cleanup_helper(scope, "@leave_cleanup"))
			return false;
		scope.emit(bc::JMP, previous_entry);

		scope.jump_here(skip);
		emit_handler_state(scope, handler, true, handler);
		scope.lbl_catchpad     = handler;
		scope.lbl_cleanup      = handler;
		scope.catchpad_cleanup = true;
		scope.cleanup_entries.push_back(entry);
		return true;
	}

	static bool parse_defer(func_scope& scope) {
		scope.lex().next();
		if (scope.lex().check('{') == lex::token_error)
			return false;
		return install_defer(scope, [](func_scope& defer_scope) {
			auto body_result = defer_scope.alloc_reg();
			auto body        = expr_block(defer_scope, body_result);
			defer_scope.free_reg(body_result);
			return body.kind != expr::err;
		});
	}

	static bool parse_type_alias(func_scope& scope) {
		scope.lex().next();
		auto name = scope.lex().check(lex::token_name);
		if (name == lex::token_error || scope.lex().check('=') == lex::token_error)
			return false;
		if (range::find_if(scope.fn.type_aliases, [&](const type_alias_entry& entry) { return string_value_equals(entry.name, name.str_val); }) !=
			 scope.fn.type_aliases.end()) {
			scope.lex().error("duplicate type alias '%s'", name.str_val->c_str());
			return false;
		}
		auto value = parse_type_name(scope);
		if (!value)
			return false;
		rc::retain(name.str_val);
		scope.fn.type_aliases.push_back({name.str_val, std::move(*value)});
		return true;
	}

	static bool skip_static_block(func_scope& scope) {
		if (scope.lex().check('{') == lex::token_error)
			return false;
		int depth = 1;
		while (scope.lex().tok != lex::token_eof) {
			if (scope.lex().tok == '{')
				++depth;
			else if (scope.lex().tok == '}') {
				--depth;
				scope.lex().next();
				if (depth == 0)
					return true;
				continue;
			}
			scope.lex().next();
		}
		scope.lex().error("unterminated static block");
		return false;
	}

	static std::optional<bool> static_boolean(func_scope& scope, const expression& value, const lex::token_value& at) {
		if (value.kind != expr::imm || !value.imm.is_bool()) {
			scope.lex().error_at(at, "static condition must be a parse-time bool");
			return std::nullopt;
		}
		return value.imm.as_bool();
	}

	static std::string static_message(const expression& value, std::string_view fallback) {
		if (value.kind == expr::imm && value.imm.is_str())
			return std::string(value.imm.as_str()->view());
		return std::string(fallback);
	}

	static expression parse_static_statement(func_scope& scope) {
		auto static_token = scope.lex().next();
		auto operation    = scope.lex().next();
		if (operation.id != lex::token_if && operation.id != lex::token_name) {
			scope.lex().error_at(operation, "expected static operation");
			return {};
		}
		const auto name = operation.id == lex::token_if ? std::string_view{"if"} : operation.str_val->view();
		if (name == "if") {
			auto condition = expr_parse(scope);
			if (condition.kind == expr::err)
				return {};
			auto selected = static_boolean(scope, condition, operation);
			if (!selected)
				return {};
			expression result{nil};
			if (*selected) {
				if (scope.lex().check('{') == lex::token_error)
					return {};
				result = expr_block(scope);
			} else if (!skip_static_block(scope)) {
				return {};
			}
			if (scope.lex().opt(lex::token_else)) {
				if (*selected) {
					if (!skip_static_block(scope))
						return {};
				} else {
					if (scope.lex().check('{') == lex::token_error)
						return {};
					result = expr_block(scope);
				}
			}
			return result;
		}

		const bool parenthesized = scope.lex().opt('(').has_value();
		auto       value         = expr_parse(scope);
		if (value.kind == expr::err)
			return {};
		expression message{nil};
		if (scope.lex().opt(',')) {
			message = expr_parse(scope);
			if (message.kind == expr::err)
				return {};
		}
		if (parenthesized && scope.lex().check(')') == lex::token_error)
			return {};

		if (name == "warn")
			return expression(nil);
		if (name == "panic") {
			auto text = static_message(value, "static panic");
			scope.lex().error_at(operation, "%s", text.c_str());
			return {};
		}
		if (name != "assert" && name != "require") {
			scope.lex().error_at(static_token, "unknown static operation '%s'", operation.str_val->c_str());
			return {};
		}
		auto condition = static_boolean(scope, value, operation);
		if (!condition)
			return {};
		if (*condition)
			return expression(nil);
		auto text = static_message(message, name == "require" ? "static requirement failed" : "static assertion failed");
		if (name == "require") {
			scope.fn.static_require_failed  = true;
			scope.fn.static_require_message = text;
			scope.fn.static_require_at      = operation;
			if (scope.fn.enclosing) {
				scope.fn.enclosing->fn.static_require_failed  = true;
				scope.fn.enclosing->fn.static_require_message = text;
				scope.fn.enclosing->fn.static_require_at      = operation;
			}
			return {};
		}
		scope.lex().error_at(operation, "%s", text.c_str());
		return {};
	}

	// Parses a "statement" expression, which considers both statements and expressions valid.
	// Returns the expression representing the value of the statement.
	//
	static expression expr_stmt(func_scope& scope, bool& fin) {
		if (!scope.fn.strict_mode && scope.lex().tok == '#') {
			const auto comment_line = scope.lex().tok.source_line;
			do {
				scope.lex().next();
			} while (scope.lex().tok != lex::token_eof && scope.lex().tok.source_line == comment_line);
			return expression(nil);
		}
		if (scope.fn.strict_mode && scope.lex().tok == lex::token_name && scope.lex().tok.str_val->view() == "static")
			return parse_static_statement(scope);
		switch (scope.lex().tok.id) {
			// Empty statement => None.
			//
			case ';': {
				return expression(nil);
			}

			// Variable declaration => None.
			//
			case lex::token_class:
			case lex::token_struct:
			case lex::token_fn:
			case lex::token_let:
			case lex::token_export:
			case lex::token_const: {
				bool is_export = scope.lex().tok.id == lex::token_export;
				if (is_export)
					scope.lex().next();
				if (!parse_decl(scope, is_export)) {
					return {};
				}
				return expression(nil);
			}

			case lex::token_type: {
				if (!parse_type_alias(scope))
					return {};
				return expression(nil);
			}

			case '[': {
				// Attribute brackets are deliberately adjacent. Inspecting the raw
				// remainder avoids consuming array-literal lookahead before the
				// statement dispatcher commits to the declaration grammar.
				if (!scope.lex().input.starts_with("["))
					return expr_parse(scope);
				table* attributes = parse_attribute_list(scope);
				if (!attributes)
					return {};
				const bool attached    = scope.lex().tok.source_line <= scope.lex().last_token_line;
				const bool file_strict = !attached && !scope.fn.enclosing && scope.locals.empty() && scope.fn.pc.empty() && scope.fn.type_aliases.empty() &&
												 attributes->active_count == 1 && has_attribute(scope.fn.L, attributes, "strict");
				if (!file_strict && (scope.lex().tok == lex::token_fn || scope.lex().tok == lex::token_class || scope.lex().tok == lex::token_struct)) {
					if (!parse_decl(scope, false, attributes))
						return {};
					return expression(nil);
				}
				if (scope.fn.enclosing || !scope.locals.empty() || !scope.fn.pc.empty() || !scope.fn.type_aliases.empty()) {
					scope.lex().error("file attributes must be the leading statement");
					return {};
				}
				scope.fn.set_attributes(attributes);
				if (has_attribute(scope.fn.L, attributes, "strict"))
					scope.fn.strict_mode = true;
				return expression(nil);
			}

			case lex::token_defer: {
				if (!parse_defer(scope))
					return {};
				return expression(nil);
			}

			// Import => None.
			//
			case lex::token_import: {
				if (scope.fn.enclosing) {
					scope.lex().error("import is only valid at top-level declarations.");
					return {};
				}
				scope.lex().next();

				bool is_optional = scope.lex().opt('?').has_value();
				auto name        = scope.lex().opt(lex::token_lstr);
				if (!name) {
					name = scope.lex().check(lex::token_name);
				}
				if (*name == lex::token_error) {
					return {};
				}

				auto alias = *name;
				if (scope.lex().opt(lex::token_as)) {
					alias = scope.lex().check(lex::token_name);
					if (alias == lex::token_error) {
						return {};
					}
				}

				// Builtin namespaces are already fully initialized VM roots and
				// can remain compile-time constants. Every recorded module,
				// including initialized and failed ones, goes through the runtime
				// helper so parsing never starts a loader or user callback.
				//
				any mod    = scope.fn.L->modules->get(scope.fn.L, (any) name->str_val);
				any record = scope.fn.L->module_records ? any(scope.fn.L->module_records->get(scope.fn.L, (any) name->str_val)) : nil;
				if (mod != nil && record == nil) {
					if (scope.fn.is_repl)
						expression{alias.str_val}.assign(scope, mod);
					else
						scope.add_local_cxpr(alias.str_val, mod);
					return expression(nil);
				}

				// Runtime import helper arguments are importer, module name, and
				// the optional-import flag. CALL consumes arguments in reverse
				// push order and adopts the helper's owned result into the local.
				//
				auto* importer = scope.fn.own(string::create(scope.fn.L, scope.lex().source_name));
				auto  result   = scope.fn.is_repl ? scope.alloc_reg() : scope.add_local(alias.str_val, true);
				expression(any(is_optional)).push(scope);
				expression(any(name->str_val)).push(scope);
				expression(any(importer)).push(scope);
				expression(nil).push(scope);
				expression(any(static_cast<function*>(&lib::fs::detail::module_import))).push(scope);
				scope.emit(bc::CALL, result, 3);
				if (scope.fn.is_repl)
					expression{alias.str_val}.assign(scope, expression(result));
				return expression(nil);
			}

			// Explicit deletion is a table-only runtime operation. Keeping it as
			// an ordinary native call gives the interpreter and JIT one semantic
			// path and never turns fixed fields or array indices into nil stores.
			//
			case lex::token_delete: {
				auto keyword = scope.lex().next();
				if (scope.lex().tok.source_line > keyword.end_line) {
					scope.lex().error("delete target must be on the same line");
					return {};
				}

				auto target = expr_primary(scope);
				if (target.kind != expr::idx) {
					scope.lex().error("delete expects a table field or index");
					return {};
				}

				std::array<expression, 2> args = {
					 expression(target.idx.table),
					 expression(target.idx.field),
				};
				auto ignored = emit_ordinary_call(scope, expression(any(static_cast<function*>(&delete_field_function))), args);
				(void) ignored;
				return expression(nil);
			}

			// Possible assignment, forward to specializer handler.
			//
			case lex::token_name: {
				return parse_assign_or_expr(scope);
			}

			// Return statement => None.
			//
			case lex::token_return: {
				if (scope.fn.atomic_parse_depth) {
					scope.lex().error("return is not valid inside atomic");
					return {};
				}
				auto keyword = scope.lex().next();
				fin          = true;
				if (scope.fn.defer_body_depth) {
					scope.lex().error("return is not valid inside defer");
					return {};
				}

				expression value{nil};
				if (auto& tk = scope.lex().tok; tk.id != ';' && tk.id != lex::token_eof && tk.id != '}' && tk.source_line <= keyword.end_line) {
					value = expr_parse(scope);
					if (value.kind == expr::err)
						return {};
				}

				value = type_check_return(scope, value);
				if (auto* cleanup = find_cleanup_scope(&scope, nullptr)) {
					value.to_reg(scope, cleanup->cleanup_value);
					scope.set_reg(cleanup->cleanup_action, any(number(cleanup_return)));
					scope.emit(bc::JMP, cleanup->cleanup_entries.back());
				} else {
					scope.emit(bc::RET, value.to_anyreg(scope));
				}
				return expression(nil);
			}

			// Throw statement => None.
			//
			case lex::token_throw: {
				auto keyword = scope.lex().next();
				fin          = true;

				// void throw:
				//
				expression res;
				if (auto& tk = scope.lex().tok; tk.id == ';' || tk.id == lex::token_eof || tk.id == '}' || tk.source_line > keyword.end_line) {
					res = expression(nil);
				}
				// value throw:
				//
				else {
					res = expr_parse(scope);
				}

				// If there is a catch pad, just jump to it, otherwise return.
				//
				auto tmp = scope.alloc_reg();
				if (res.kind == expr::reg) {
					scope.emit(bc::SETEX, res.reg);
				} else {
					res.to_reg(scope, tmp);
					scope.emit(bc::SETEX, tmp);
				}

				if (scope.lbl_catchpad != 0) {
					scope.emit(bc::JMP, scope.lbl_catchpad);
				} else {
					scope.set_reg(tmp, exception_marker);
					scope.emit(bc::RET, tmp);
				}
				scope.free_reg(tmp);
				return expression(nil);
			}

			// Continue/Break/Leave.
			//
			case lex::token_continue: {
				scope.lex().next();
				fin = true;

				if (!scope.lbl_continue) {
					scope.lex().error("no loop to continue from");
					return {};
				}
				if (scope.fn.defer_body_depth) {
					scope.lex().error("continue is not valid inside defer");
					return {};
				}
				if (!emit_cleanup_transfer(scope, &scope, scope.owner_continue, cleanup_continue))
					scope.emit(bc::JMP, scope.lbl_continue);
				return expression(nil);
			}
			case lex::token_break: {
				auto keyword = scope.lex().next();
				fin          = true;

				if (!scope.lbl_break) {
					scope.lex().error("no loop/switch to break from");
					return {};
				}

				// void return:
				//
				if (auto& tk = scope.lex().tok; tk.id == ';' || tk.id == lex::token_eof || tk.id == '}' || tk.source_line > keyword.end_line) {
					if (scope.fn.defer_body_depth) {
						scope.lex().error("break is not valid inside defer");
						return {};
					}
					if (!emit_cleanup_transfer(scope, &scope, scope.owner_break, cleanup_break))
						scope.emit(bc::JMP, scope.lbl_break);
				}
				// value return:
				//
				else {
					auto value = expr_parse(scope);
					if (value.kind == expr::err) {
						return {};
					}
					value.to_reg(scope, scope.reg_break);
					if (scope.fn.defer_body_depth) {
						scope.lex().error("break is not valid inside defer");
						return {};
					}
					if (!emit_cleanup_transfer(scope, &scope, scope.owner_break, cleanup_break))
						scope.emit(bc::JMP, scope.lbl_break);
				}
				return expression(nil);
			}
			case lex::token_leave: {
				auto keyword = scope.lex().next();
				fin          = true;

				if (!scope.lbl_leave) {
					scope.lex().error("no block expression to leave from");
					return {};
				}

				if (auto& tk = scope.lex().tok; tk.id == ';' || tk.id == lex::token_eof || tk.id == '}' || tk.source_line > keyword.end_line) {
					scope.set_reg(scope.reg_leave, nil);
				} else {
					auto value = expr_parse(scope);
					if (value.kind == expr::err) {
						return {};
					}
					value.to_reg(scope, scope.reg_leave);
				}
				if (scope.fn.defer_body_depth) {
					scope.lex().error("leave is not valid inside defer");
					return {};
				}
				auto* leave_end = scope.owner_leave ? scope.owner_leave->prev : nullptr;
				if (!emit_cleanup_transfer(scope, &scope, leave_end, cleanup_leave))
					scope.emit(bc::JMP, scope.lbl_leave);
				return expression(nil);
			}

			// Anything else gets forward to expression parser.
			//
			default: {
				return expr_parse(scope);
			}
		}
	}

	using generated_block_setup = bool (*)(func_scope&, void*);

	// Parses a block expression made up of N statements, final one is the expression value
	// unless closed with a semi-colon. A generated setup runs in the block's
	// lexical scope before its first statement and can install cleanup entries.
	//
	static expression expr_block_impl(func_scope& pscope, bc::reg into, bool no_term, generated_block_setup setup, void* setup_context) {
		if (!setup && pscope.lex().tok == '}') {
			if (!no_term)
				pscope.lex().next();
			if (into != -1) {
				pscope.set_reg(into, nil);
				return expression(into, false, strict::typespec::nil());
			}
			return expression(nil);
		}

		bc::reg          result      = into;
		strict::typespec result_type = strict::typespec::nil();
		{
			bool       fin = false;
			func_scope scope{pscope.fn};
			bc::reg    scope_base = scope.reg_next;
			if (result == -1)
				result = scope.alloc_reg();
			scope.lbl_leave   = scope.make_label();
			scope.reg_leave   = result;
			scope.owner_leave = &scope;
			if (setup && !setup(scope, setup_context))
				return {};

			expression last{nil};
			if (scope.lex().tok == '}') {
				if (!no_term)
					scope.lex().next();
			} else {
				while (true) {
					// If finalized but block is not closed, fail.
					//
					if (fin) {
						pscope.lex().error("unreachable statement");
						return {};
					}

					// Parse a statement, forward error.
					//
					auto statement_base = scope.reg_next;
					last                = expr_stmt(scope, fin);
					if (last.kind == expr::err)
						return {};

					// If closed with semi-colon, clear value.
					//
					bool terminated = scope.lex().opt(';').has_value();
					if (!terminated && scope.lex().tok != '}' && scope.lex().tok.source_line <= scope.lex().last_token_line) {
						scope.lex().error("expected ';' or newline between statements");
						return {};
					}
					if (terminated) {
						clear_dead_registers(scope, statement_base);
						last = nil;
					}
					if (scope.lex().tok == '}') {
						if (!no_term)
							scope.lex().next();
						break;
					}

					// This statement cannot become the block value. Release every
					// scratch slot it created while leaving declared locals alive.
					//
					if (!terminated) {
						clear_dead_registers(scope, statement_base);
						last = nil;
					}
				}
			}

			result_type = last.static_type;
			if (!last.validate_union_read(scope))
				return {};
			last.to_reg(scope, result);
			if (!scope.cleanup_entries.empty()) {
				scope.set_reg(scope.cleanup_action, any(number(cleanup_normal)));
				scope.emit(bc::JMP, scope.cleanup_entries.back());
				emit_cleanup_dispatch(scope, scope.lbl_leave);
			}
			scope.set_label_here(scope.lbl_leave);
			scope.discard_regs(into == -1 ? result + 1 : scope_base);
		}

		// An implicit target is allocated in the child at the parent's next
		// register, so reserve that same register when the value escapes.
		//
		if (into == -1)
			pscope.alloc_reg();
		return expression(result, false, std::move(result_type));
	}

	static expression expr_block(func_scope& scope, bc::reg into, bool no_term) { return expr_block_impl(scope, into, no_term, nullptr, nullptr); }

	struct lock_block_context {
		bc::reg target;
	};

	static bool setup_lock_block(func_scope& scope, void* opaque) {
		const auto                target      = static_cast<lock_block_context*>(opaque)->target;
		std::array<expression, 1> lock_args   = {expression(target)};
		auto                      lock_result = emit_ordinary_call(scope, expression(any(static_cast<function*>(&atomic::detail::lock_shared))), lock_args);
		scope.free_reg(lock_result.reg);

		return install_defer(scope, [target](func_scope& cleanup_scope) {
			std::array<expression, 1> unlock_args = {expression(target)};
			auto unlock_result = emit_ordinary_call(cleanup_scope, expression(any(static_cast<function*>(&atomic::detail::unlock_shared))), unlock_args);
			cleanup_scope.free_reg(unlock_result.reg);
			return true;
		});
	}

	static expression parse_lock(func_scope& scope) {
		++scope.fn.block_terminator_depth;
		auto target = expr_parse(scope);
		--scope.fn.block_terminator_depth;
		if (target.kind == expr::err)
			return {};
		if (scope.lex().check('{') == lex::token_error)
			return {};

		const auto         target_reg = target.to_nextreg(scope);
		const auto         result_reg = scope.alloc_reg();
		lock_block_context context{target_reg};
		++scope.fn.lexical_lock_depth;
		auto body = expr_block_impl(scope, result_reg, false, &setup_lock_block, &context);
		--scope.fn.lexical_lock_depth;
		if (body.kind == expr::err)
			return {};

		scope.emit(bc::MOV, target_reg, result_reg);
		scope.free_reg(result_reg);
		return expression(target_reg);
	}

	static expression make_atomic_closure(func_scope& scope, function* prototype_function, const func_state& plan) {
		if (prototype_function->num_uval == 0)
			return expression(any(prototype_function));

		const auto captures = scope.alloc_reg(msize_t(plan.uvalues.size()));
		for (size_t index = 0; index != plan.uvalues.size(); ++index)
			expr_var(scope, plan.uvalues[index].id).to_reg(scope, captures + bc::reg(index));
		scope.emit(bc::FDUP, captures, scope.add_const(any(prototype_function)).first, captures);
		if (plan.uvalues.size() > 1)
			scope.free_reg(captures + 1, msize_t(plan.uvalues.size() - 1));
		return expression(captures);
	}

	static expression parse_atomic(func_scope& scope) {
		if (scope.lex().check('{') == lex::token_error)
			return {};

		const auto line = scope.lex().line;
		func_state plan{scope.fn, scope};
		plan.disable_jit        = true;
		plan.atomic_parse_depth = 1;
		{
			func_scope plan_scope{plan};
			auto       value = expr_block(plan_scope);
			if (value.kind == expr::err)
				return {};
			value.to_reg(plan_scope, 0);
		}

		function* transaction = write_func(plan, line, bc::reg(0));
		if (!transaction)
			return {};
		scope.fn.own(transaction);
		if (const char* error = atomic::validate_plan(transaction->proto)) {
			scope.lex().error("%s", error);
			return {};
		}

		std::vector<bool> changed(plan.uvalues.size(), false);
		for (const bc::insn& instruction : transaction->proto->opcodes()) {
			if (instruction.o == bc::USET)
				changed[size_t(instruction.a)] = true;
		}

		auto                      closure      = make_atomic_closure(scope, transaction, plan);
		std::array<expression, 1> execute_args = {closure};
		auto                      result       = emit_ordinary_call(scope, expression(any(static_cast<function*>(&atomic::detail::execute))), execute_args);

		for (size_t index = 0; index != changed.size(); ++index) {
			if (!changed[index])
				continue;
			auto destination = expr_var(scope, plan.uvalues[index].id);
			if (!ensure_assignable(scope, destination))
				return {};
			std::array<expression, 2> capture_args = {
				 closure,
				 expression(any(number(index))),
			};
			auto value = emit_ordinary_call(scope, expression(any(static_cast<function*>(&atomic::detail::capture))), capture_args);
			destination.assign(scope, value);
			scope.free_reg(value.reg);
		}

		if (closure.kind == expr::reg) {
			result.to_reg(scope, closure.reg);
			scope.discard_regs(closure.reg + 1);
			return expression(closure.reg);
		}
		return result;
	}

	// Parses script body.
	//
	static bool parse_body(func_scope scope) {
		expression last{nil};
		bool       fin = false;
		while (scope.lex().tok != lex::token_eof) {
			// If finalized but block is not closed, fail.
			//
			if (fin) {
				scope.lex().error("unreachable statement");
				return false;
			}

			// Parse a statement.
			//
			auto statement_base = scope.reg_next;
			if (last = expr_stmt(scope, fin); last.kind == expr::err) {
				return false;
			}

			// If closed with semi-colon, clear value.
			//
			bool terminated = scope.lex().opt(';').has_value();
			if (!terminated && scope.lex().tok != lex::token_eof && scope.lex().tok.source_line <= scope.lex().last_token_line) {
				scope.lex().error("expected ';' or newline between statements");
				return false;
			}
			if (terminated) {
				clear_dead_registers(scope, statement_base);
				last = nil;
			} else if (scope.lex().tok != lex::token_eof) {
				clear_dead_registers(scope, statement_base);
				last = nil;
			}
		}

		// Yield last value after root-scope cleanup.
		//
		if (!scope.cleanup_entries.empty()) {
			last.to_reg(scope, scope.cleanup_value);
			auto done = scope.make_label();
			scope.set_reg(scope.cleanup_action, any(number(cleanup_normal)));
			scope.emit(bc::JMP, scope.cleanup_entries.back());
			emit_cleanup_dispatch(scope, done);
			scope.set_label_here(done);
			scope.emit(bc::RET, scope.cleanup_value);
		} else {
			scope.emit(bc::RET, last.to_anyreg(scope));
		}
		return true;
	}

	static bool same_type_tuple(std::span<const strict::typespec> left, std::span<const strict::typespec> right) {
		if (left.size() != right.size())
			return false;
		for (size_t index = 0; index != left.size(); ++index) {
			if (!strict::same(left[index], right[index]))
				return false;
		}
		return true;
	}

	struct generic_match {
		generic_candidate*                         candidate = nullptr;
		std::vector<strict::typespec>              bindings;
		std::vector<std::vector<strict::typespec>> packs;
		std::vector<strict::typespec>              key;
		size_t                                     candidate_index = 0;
		int                                        score           = 0;
	};

	static std::optional<generic_match> match_generic_candidate(
		 generic_candidate& candidate, std::span<const strict::typespec> explicit_types, std::span<const expression> arguments) {
		generic_match result;
		result.candidate = &candidate;
		result.bindings.resize(candidate.parameters.size(), strict::typespec::generic(std::numeric_limits<uint32_t>::max()));
		result.packs.resize(candidate.parameters.size());

		size_t variadic_index = candidate.parameters.size();
		for (size_t index = 0; index != candidate.parameters.size(); ++index) {
			if (candidate.parameters[index].variadic) {
				variadic_index = index;
				break;
			}
		}
		if (!explicit_types.empty()) {
			if (variadic_index == candidate.parameters.size() && explicit_types.size() != candidate.parameters.size())
				return std::nullopt;
			if (variadic_index != candidate.parameters.size() && explicit_types.size() < variadic_index)
				return std::nullopt;
			for (size_t index = 0; index != std::min(explicit_types.size(), variadic_index); ++index)
				result.bindings[index] = explicit_types[index];
			if (variadic_index != candidate.parameters.size()) {
				result.packs[variadic_index].assign(explicit_types.begin() + variadic_index, explicit_types.end());
				if (!result.packs[variadic_index].empty())
					result.bindings[variadic_index] = result.packs[variadic_index].front();
			}
		} else if (!candidate.parameter_patterns.empty()) {
			const size_t fixed = candidate.variadic_call_pattern ? candidate.parameter_patterns.size() - 1 : candidate.parameter_patterns.size();
			if ((!candidate.variadic_call_pattern && arguments.size() != fixed) || arguments.size() < fixed)
				return std::nullopt;
			for (size_t index = 0; index != fixed; ++index) {
				if (!strict::bind(candidate.parameter_patterns[index], arguments[index].static_type, result.bindings))
					return std::nullopt;
			}
			if (candidate.variadic_call_pattern) {
				const auto& pattern = candidate.parameter_patterns.back();
				if (pattern.kind != strict::type_kind::generic_param || pattern.generic_index >= candidate.parameters.size())
					return std::nullopt;
				auto pack_index = size_t(pattern.generic_index);
				for (size_t index = fixed; index != arguments.size(); ++index)
					result.packs[pack_index].push_back(arguments[index].static_type);
				if (!result.packs[pack_index].empty())
					result.bindings[pack_index] = result.packs[pack_index].front();
			}
		} else if (!candidate.parameters.empty()) {
			if (arguments.empty())
				return std::nullopt;
			if (variadic_index == candidate.parameters.size()) {
				for (size_t index = 0; index != candidate.parameters.size(); ++index)
					result.bindings[index] = arguments[std::min(index, arguments.size() - 1)].static_type;
			} else {
				for (const auto& argument : arguments)
					result.packs[variadic_index].push_back(argument.static_type);
				if (!result.packs[variadic_index].empty())
					result.bindings[variadic_index] = result.packs[variadic_index].front();
			}
		}

		for (size_t index = 0; index != candidate.parameters.size(); ++index) {
			const auto& parameter = candidate.parameters[index];
			if (parameter.variadic) {
				for (const auto& value : result.packs[index]) {
					if (parameter.constraint && !strict::same(*parameter.constraint, value))
						return std::nullopt;
					result.key.push_back(value);
				}
				result.score += parameter.constraint ? 1000 : 0;
				continue;
			}
			if (result.bindings[index].kind == strict::type_kind::generic_param && result.bindings[index].generic_index == std::numeric_limits<uint32_t>::max())
				return std::nullopt;
			if (parameter.constraint && !strict::same(*parameter.constraint, result.bindings[index]))
				return std::nullopt;
			result.key.push_back(result.bindings[index]);
			result.score += parameter.constraint ? 1000 : 0;
		}
		for (const auto& pattern : candidate.parameter_patterns)
			result.score += strict::specificity(pattern);
		return result;
	}

	static bool rebuild_generic_dispatcher(func_scope& scope, generic_template& declaration) {
		function* target = declaration.declaration_value;
		if (!target || !target->proto)
			return true;

		func_state dispatcher{scope.fn, scope};
		dispatcher.is_vararg = true;
		dispatcher.set_attributes(target->proto->attributes);
		dispatcher.pc.emplace_back();
		{
			func_scope body{dispatcher};
			dispatcher.pc.front().o = bc::VACHK;
			dispatcher.pc.front().a = 0;
			auto* argument_error    = dispatcher.own(string::create(dispatcher.L, "generic dispatch accepts a variadic argument list"));
			dispatcher.pc.front().set_xmm(body.add_const(argument_error).second.value);

			for (const auto& record : declaration.instantiations) {
				if (record.status != generic_instantiation_status::ok || !record.value.is_fn() || !record.value.as_fn()->proto ||
					 !record.value.as_fn()->proto->signature)
					continue;
				auto*       specialized = record.value.as_fn();
				const auto& parameters  = specialized->proto->signature->parameters;
				auto        next        = body.make_label();
				auto        count       = body.alloc_reg(2);
				body.emit(bc::VACNT, count);
				body.set_reg(count + 1, any(number(parameters.size())));
				body.emit(bc::CEQ, count, count, count + 1);
				body.emit(bc::JNS, next, count);
				body.free_reg(count, 2);

				for (size_t index = 0; index != parameters.size(); ++index) {
					reg_sweeper sweep{body};
					auto        argument = body.alloc_reg();
					body.set_reg(argument, any(number(index)));
					body.emit(bc::VAGET, argument, argument);
					auto check = emit_type_test(body, expression(argument), parameters[index]);
					if (check.kind == expr::err)
						return false;
					body.emit(bc::JNS, next, check.to_anyreg(body));
				}

				auto result = body.alloc_reg(2);
				for (size_t index = parameters.size(); index != 0; --index) {
					auto argument = body.alloc_reg();
					body.set_reg(argument, any(number(index - 1)));
					body.emit(bc::VAGET, argument, argument);
					body.emit(bc::PUSHR, argument);
					body.free_reg(argument);
				}
				expression(bc::reg(FRAME_SELF), true).push(body);
				expression(any(specialized)).push(body);
				body.emit(bc::CALL, result, bc::reg(parameters.size()));
				body.emit(bc::RET, result);
				body.set_label_here(next);
				body.discard_regs(0);
			}
			auto error = body.alloc_reg();
			body.set_reg(error,
				 dispatcher.own(string::format(dispatcher.L, "no cached generic specialization of %s matches runtime arguments", declaration.name->c_str())));
			body.emit(bc::SETEX, error);
			body.set_reg(error, exception_marker);
			body.emit(bc::RET, error);
		}

		function* replacement = write_func(dispatcher, declaration.candidates.empty() ? 0 : declaration.candidates.front().source.line);
		if (!replacement)
			return false;
		auto metadata               = target->proto->generic;
		replacement->proto->generic = std::move(metadata);
		rc::retain(replacement->proto);
		auto* previous   = target->proto;
		target->proto    = replacement->proto;
		target->num_uval = 0;
		rc::release(scope.fn.L, previous);
		rc::release(scope.fn.L, replacement);
		return true;
	}

	static void patch_forward_class(vm* L, function* method, vclass* forward, vclass* actual) {
		if (!method)
			return;
		auto patch_value = [&](any& value) {
			if (value.is_vcl() && value.as_vcl() == forward) {
				rc::retain(actual);
				rc::release(L, value);
				value = any(actual);
			}
		};
		for (any& upvalue : method->uvals())
			patch_value(upvalue);
		if (!method->proto)
			return;
		for (any& constant : method->proto->kvals()) {
			patch_value(constant);
			if (constant.is_fn() && constant.as_fn() != method)
				patch_forward_class(L, constant.as_fn(), forward, actual);
		}
	}

	static void patch_forward_class(vm* L, vclass* actual, vclass* forward) {
		patch_forward_class(L, actual->ctor, forward, actual);
		patch_forward_class(L, actual->initializer, forward, actual);
		patch_forward_class(L, actual->constructor_body, forward, actual);
		for (const auto& field : actual->fields()) {
			if (!field.value.is_static || field.value.ty != type::fn)
				continue;
			auto method = any::load_from(actual->static_space() + field.value.offset, type::fn);
			if (method.is_fn())
				patch_forward_class(L, method.as_fn(), forward, actual);
		}
		for (const auto& property : actual->properties)
			patch_forward_class(L, property.method, forward, actual);
		if (actual->traits) {
			for (function* method : actual->traits->methods)
				patch_forward_class(L, method, forward, actual);
		}
	}

	static expression expression_for_instantiation(any value) {
		if (value.is_fn()) {
			expression result(value, function_static_type(value.as_fn()));
			return result;
		}
		if (value.is_vcl()) {
			auto* declared = value.as_vcl();
			return expression(value, declared->value_semantics ? strict::typespec::struct_type(declared) : strict::typespec::class_type(declared));
		}
		return {};
	}

	static expression instantiate_generic(func_scope& scope, generic_template& declaration, std::span<const strict::typespec> explicit_types,
		 std::span<const expression> arguments, const lex::token_value& call_token) {
		std::vector<generic_match> matches;
		for (size_t candidate_index = 0; candidate_index != declaration.candidates.size(); ++candidate_index) {
			if (auto match = match_generic_candidate(declaration.candidates[candidate_index], explicit_types, arguments)) {
				match->candidate_index = candidate_index;
				matches.push_back(std::move(*match));
			}
		}
		if (matches.empty()) {
			scope.lex().error_at(call_token, "cannot resolve generic parameters for %s", declaration.name->c_str());
			return {};
		}
		std::sort(matches.begin(), matches.end(), [](const generic_match& left, const generic_match& right) { return left.score > right.score; });

		std::string require_failure;
		for (size_t group_begin = 0; group_begin != matches.size();) {
			size_t group_end = group_begin + 1;
			while (group_end != matches.size() && matches[group_end].score == matches[group_begin].score)
				++group_end;
			std::vector<expression> successful;
			for (size_t match_index = group_begin; match_index != group_end; ++match_index) {
				auto& match = matches[match_index];
				for (const auto& active : declaration.classes_in_progress) {
					if (active.forward && same_type_tuple(active.types, match.key)) {
						strict::typespec instance_type =
							 active.forward->value_semantics ? strict::typespec::struct_type(active.forward) : strict::typespec::class_type(active.forward);
						if (scope.fn.class_decl_identity == active.forward->identity) {
							std::array<expression, 1> self_argument = {expression(bc::reg(FRAME_SELF), true, strict::typespec::self(active.forward))};
							auto runtime_class        = emit_ordinary_call(scope, expression(any(static_cast<function*>(&class_of_function))), self_argument);
							runtime_class.static_type = instance_type;
							successful.push_back(runtime_class);
						} else {
							successful.push_back(expression(any(active.forward), instance_type));
						}
						goto candidate_done;
					}
				}
				for (size_t cached_index = 0, cached_count = declaration.instantiations.size(); cached_index != cached_count; ++cached_index) {
					const auto& cached = declaration.instantiations[cached_index];
					if (cached.status == generic_instantiation_status::ok && cached.candidate_index == match.candidate_index &&
						 same_type_tuple(cached.types, match.key)) {
						any cached_value = cached.value;
						rc::retain(cached_value);
						declaration.instantiations.push_back({match.key, generic_instantiation_status::cache_ok, cached_value, {}, match.candidate_index});
						successful.push_back(expression_for_instantiation(cached_value));
						goto candidate_done;
					}
				}

				{
					lex::state     caller                 = scope.fn.lex;
					const uint32_t caller_last_lexed_line = scope.fn.last_lexed_line;
					scope.fn.lex                          = match.candidate->source;
					scope.fn.lex.last_error.clear();
					scope.fn.lex.frames         = caller.frames;
					scope.fn.lex.verbose_errors = caller.verbose_errors;
					scope.fn.lex.frames.push_back(
						 {util::fmt("called from %.*s:%u", int(caller.source_name.size()), caller.source_name.data(), call_token.source_line),
							  call_token.source_line});
					std::string instance_name = declaration.name->c_str();
					instance_name += '<';
					for (size_t index = 0; index != match.key.size(); ++index) {
						if (index)
							instance_name += ", ";
						instance_name += strict::to_string(match.key[index]);
					}
					instance_name += '>';
					scope.fn.lex.frames.push_back({"in instantiation of " + instance_name, match.candidate->source.tok.source_line});
					auto*    instantiated_name  = scope.fn.own(string::create(scope.fn.L, instance_name));
					vclass*  recursive_forward  = nullptr;
					uint64_t recursive_identity = 0;
					if (match.candidate->class_template) {
						recursive_identity = reserve_class_identity(scope.fn.L);
						recursive_forward =
							 scope.fn.own(vclass::create(scope.fn.L, instantiated_name, {}, {}, {}, nullptr, recursive_identity, match.candidate->value_semantics));
						declaration.classes_in_progress.push_back({match.key, recursive_forward});
					}

					const size_t alias_base           = scope.fn.type_aliases.size();
					const size_t pack_base            = scope.fn.variadic_type_aliases.size();
					string*      saved_class_name     = scope.fn.class_decl_name;
					uint64_t     saved_class_identity = scope.fn.class_decl_identity;
					bool         saved_class_strict   = scope.fn.class_strict_mode;
					auto*        saved_class_fields   = scope.fn.class_field_types;
					auto*        saved_active_generic = scope.fn.active_generic;
					scope.fn.active_generic           = &declaration;
					if (match.candidate->class_decl_identity) {
						scope.fn.class_decl_name     = match.candidate->class_decl_name;
						scope.fn.class_decl_identity = match.candidate->class_decl_identity;
						scope.fn.class_strict_mode   = true;
						auto nominal                 = range::find_if(scope.fn.nominal_types, [&](const nominal_type_binding& binding) {
							return binding.declared && binding.declared->identity == match.candidate->class_decl_identity;
						});
						scope.fn.class_field_types   = nominal == scope.fn.nominal_types.end() ? nullptr : &nominal->fields;
					}
					for (size_t index = 0; index != match.candidate->parameters.size(); ++index) {
						const auto& parameter = match.candidate->parameters[index];
						if (parameter.variadic) {
							rc::retain(parameter.name);
							scope.fn.variadic_type_aliases.push_back({parameter.name, match.packs[index]});
							if (!match.packs[index].empty()) {
								rc::retain(parameter.name);
								scope.fn.type_aliases.push_back({parameter.name, strict::typespec::union_of(match.packs[index])});
							}
						} else {
							rc::retain(parameter.name);
							scope.fn.type_aliases.push_back({parameter.name, match.bindings[index]});
						}
					}
					scope.fn.static_require_failed = false;
					scope.fn.static_require_message.clear();
					expression value = match.candidate->class_template
												  ? parse_class(scope, instantiated_name, match.candidate->value_semantics, match.candidate->attributes, recursive_identity)
												  : parse_function(scope, instantiated_name, nullptr, match.candidate->attributes);
					const bool sfinae      = scope.fn.static_require_failed;
					std::string failure    = scope.fn.static_require_message.empty() ? scope.fn.lex.last_error : scope.fn.static_require_message;
					std::string diagnostic = scope.fn.lex.last_error;
					while (scope.fn.type_aliases.size() != alias_base) {
						rc::release(scope.fn.L, scope.fn.type_aliases.back().name);
						scope.fn.type_aliases.pop_back();
					}
					while (scope.fn.variadic_type_aliases.size() != pack_base) {
						rc::release(scope.fn.L, scope.fn.variadic_type_aliases.back().name);
						scope.fn.variadic_type_aliases.pop_back();
					}
					scope.fn.lex                 = std::move(caller);
					scope.fn.last_lexed_line     = caller_last_lexed_line;
					scope.fn.class_decl_name     = saved_class_name;
					scope.fn.class_decl_identity = saved_class_identity;
					scope.fn.class_strict_mode   = saved_class_strict;
					scope.fn.class_field_types   = saved_class_fields;
					scope.fn.active_generic      = saved_active_generic;
					if (recursive_forward) {
						LI_ASSERT(!declaration.classes_in_progress.empty() && declaration.classes_in_progress.back().forward == recursive_forward);
						declaration.classes_in_progress.pop_back();
					}
					if (value.kind == expr::err || sfinae) {
						generic_instantiation_record failed{match.key, generic_instantiation_status::failed, nil, failure, match.candidate_index};
						declaration.instantiations.push_back(std::move(failed));
						if (sfinae) {
							require_failure = failure;
							goto candidate_done;
						}
						if (scope.fn.lex.last_error.empty())
							scope.fn.lex.last_error = std::move(diagnostic);
						return {};
					}
					if (recursive_forward && value.imm.is_vcl()) {
						patch_forward_class(scope.fn.L, value.imm.as_vcl(), recursive_forward);
						rc::retain(recursive_forward);
						declaration.recursive_forwards.push_back(recursive_forward);
					}
					rc::retain(value.imm);
					declaration.instantiations.push_back({match.key, generic_instantiation_status::ok, value.imm, {}, match.candidate_index});
					if (!rebuild_generic_dispatcher(scope, declaration))
						return {};
					successful.push_back(value);
				}
			candidate_done:;
			}
			if (successful.size() > 1) {
				scope.lex().error_at(call_token, "ambiguous generic instantiation of %s", declaration.name->c_str());
				return {};
			}
			if (successful.size() == 1)
				return successful.front();
			group_begin = group_end;
		}
		if (!require_failure.empty())
			scope.lex().error_at(call_token, "%s", require_failure.c_str());
		else
			scope.lex().error_at(call_token, "no viable generic candidate for %s", declaration.name->c_str());
		return {};
	}

	// Parses a call, returns the result.
	//
	static expression parse_call(func_scope& scope, const expression& func, const expression& self, std::span<const strict::typespec> explicit_types) {
		if (func.kind == expr::err || self.kind == expr::err)
			return {};
		const auto call_token = scope.lex().tok;

		expression callsite[MAX_ARGS] = {};
		msize_t    size               = 0;
		expression callee             = func;
		expression receiver           = self;

		// Allocate temporary site for result.
		//
		bc::reg tmp = scope.alloc_reg(2);

		// Collect arguments.
		//
		if (auto lit = scope.lex().opt('{')) {
			auto argument = expr_table(scope);
			if (argument.kind == expr::err)
				return {};
			callsite[size++] = argument;
		} else if (auto lit = scope.lex().opt(lex::token_lstr)) {
			callsite[size++] = expression(any(lit->str_val));
		} else {
			if (scope.lex().opt('!')) {
				scope.lex().error("postfix '!' trait installation was removed; declare hooks in a class or use the traits module");
				return {};
			}

			continuation_scope continuation{scope.fn};
			if (scope.lex().check('(') == lex::token_error)
				return {};
			if (!scope.lex().opt(')')) {
				while (true) {
					if (size == std::size(callsite)) {
						scope.lex().error("too many arguments");
						return {};
					}

					// If reference:
					//
					if (auto ex = expr_parse(scope); ex.kind == expr::err)
						return {};
					else
						callsite[size++] = ex;

					if (scope.lex().opt(')'))
						break;
					else if (scope.lex().check(',') == lex::token_error)
						return {};
				}
			}
		}

		std::shared_ptr<generic_template> generic_owner;
		generic_template*                 generic = callee.generic_owner;
		if (callee.kind == expr::imm && callee.imm.is_fn() && callee.imm.as_fn()->proto) {
			generic_owner = callee.imm.as_fn()->proto->generic;
			if (generic_owner)
				generic = generic_owner.get();
		} else if (callee.kind == expr::imm && callee.imm.is_vcl()) {
			generic_owner = callee.imm.as_vcl()->generic;
			if (generic_owner)
				generic = generic_owner.get();
		} else if (receiver.kind == expr::imm && receiver.imm.is_vcl()) {
			generic_owner = receiver.imm.as_vcl()->generic;
			if (generic_owner)
				generic = generic_owner.get();
		}
		if (generic) {
			auto instantiated = instantiate_generic(scope, *generic, explicit_types, std::span<const expression>(callsite, size), call_token);
			if (instantiated.kind == expr::err)
				return {};
			if (receiver.kind == expr::imm && receiver.imm.is_vcl() && receiver.imm.as_vcl()->generic.get() == generic)
				receiver = instantiated;
			else
				callee = instantiated;
		}

		strict::typespec result_type = strict::typespec::any();
		if (callee.kind == expr::imm && callee.imm.is_vcl()) {
			auto* declared = callee.imm.as_vcl();
			result_type    = declared->value_semantics ? strict::typespec::struct_type(declared) : strict::typespec::class_type(declared);
		} else if (receiver.kind == expr::imm && receiver.imm.is_vcl()) {
			auto* declared = receiver.imm.as_vcl();
			result_type    = declared->value_semantics ? strict::typespec::struct_type(declared) : strict::typespec::class_type(declared);
		} else if (callee.kind == expr::imm && callee.imm.is_fn() && callee.imm.as_fn() == static_cast<function*>(&lib::detail::builtin_class_new) &&
					  (receiver.static_type.kind == strict::type_kind::class_ref || receiver.static_type.kind == strict::type_kind::struct_ref)) {
			result_type = receiver.static_type;
		} else if (callee.static_type.kind == strict::type_kind::function && !callee.static_type.children.empty()) {
			const size_t parameter_count = callee.static_type.children.size() - 1;
			result_type                  = callee.static_type.children.back();
			for (size_t index = 0; index != size && index != parameter_count; ++index) {
				const auto& expected = callee.static_type.children[index];
				if (!strict_assignable(scope, expected, callsite[index], util::fmt("argument %u", unsigned(index + 1))))
					return {};
			}
		}

		// Create the callsite.
		//
		for (bc::reg i = (size - 1); i >= 0; i--)
			callsite[i].push(scope);
		receiver.push(scope);
		callee.push(scope);
		scope.emit(bc::CALL, tmp, size);
		scope.discard_regs(tmp + 1);
		expression result(tmp, false, std::move(result_type));
		result.fresh_struct_value = result.static_type.kind == strict::type_kind::struct_ref;
		return result;
	}

	// Parses a format string and returns the result.
	//
	static expression parse_format(func_scope& scope) {
		expression parts[64];
		msize_t    size = 0;

		// Parse format string.
		//
		auto fmt = scope.lex().next().str_val->view();
		auto add = [&](expression e) -> bool {
			// If literal, coerce to string:
			//
			if (e.kind == expr::imm && !e.imm.is_str()) {
				e.imm = scope.fn.own(any(e.imm.to_string(scope.fn.L)));
			}

			// Try merging with the previous instance.
			//
			if (e.kind == expr::imm && size && parts[size - 1].kind == expr::imm) {
				parts[size - 1] = scope.fn.own(any(string::concat(scope.fn.L, parts[size - 1].imm.as_str(), e.imm.as_str())));
				return true;
			}

			// Size check.
			//
			if (size == std::size(parts)) {
				scope.lex().error("format string too complex");
				return false;
			}

			// Append and return.
			//
			parts[size++] = e;
			return true;
		};
		while (!fmt.empty()) {
			// If we reached the end, push the rest as literal.
			//
			if (auto next = fmt.find_first_of("{}"); next == std::string::npos) {
				if (!add(scope.fn.own(any(string::create(scope.fn.L, fmt))))) {
					return {};
				}
				break;
			}
			// Otherwise push the leftover.
			//
			else if (next != 0) {
				if (!add(scope.fn.own(any(string::create(scope.fn.L, fmt.substr(0, next)))))) {
					return {};
				}
				fmt.remove_prefix(next);
			}

			// Make sure it is properly terminated.
			//
			if (fmt.size() == 1) {
				scope.lex().error("unterminated format string");
				return {};
			}

			// Skip if escaped.
			//
			if (fmt[1] == fmt[0]) {
				if (!add(scope.fn.own(any(string::create(scope.fn.L, fmt.substr(0, 1)))))) {
					return {};
				}
				fmt.remove_prefix(2);
				continue;
			}

			// Unmatched block?
			//
			if (fmt[0] == '}') {
				scope.lex().error("unmatched block within format string");
				return {};
			}

			// Parse a block expression absuing lexer buffer.
			//
			fmt.remove_prefix(1);
			auto [pi, pt, pl]    = std::tuple(scope.lex().input, scope.lex().tok, std::move(scope.lex().tok_lookahead));
			auto source_line     = scope.lex().line;
			auto last_token_line = scope.lex().last_token_line;
			scope.fn.lex.input   = fmt;
			scope.fn.lex.tok     = scope.fn.lex.scan();
			scope.fn.lex.tok_lookahead.reset();

			auto expr = expr_block(scope, -1, true);
			LI_ASSERT(!scope.lex().tok_lookahead);

			fmt                         = scope.lex().input;
			scope.lex().input           = pi;
			scope.lex().tok             = pt;
			scope.lex().tok_lookahead   = std::move(pl);
			scope.lex().line            = source_line;
			scope.lex().last_token_line = last_token_line;

			if (expr.kind == expr::err || !add(std::move(expr)))
				return {};
		}

		// Allocate base of concat.
		//
		auto r = scope.alloc_reg(size);
		for (msize_t i = 0; i != size; i++) {
			parts[i].to_reg(scope, r + i);
		}
		scope.emit(bc::CCAT, r, (int32_t) size);

		// Free the rest of the registers and return the result.
		//
		scope.discard_regs(r + 1);
		return expression(r, false, strict::typespec::string_type());
	}

	// Parses function declaration, returns the function value.
	//
	static expression parse_function(func_scope& scope, string* name, function_shape* shape, table* attributes) {
		msize_t line_num = scope.lex().line;
		auto    id       = scope.lex().next().id;

		msize_t    opt_count = 0;
		func_state new_fn{scope.fn, scope};
		new_fn.set_decl_name(name);
		new_fn.set_attributes(attributes);
		if (has_attribute(scope.fn.L, attributes, "strict"))
			new_fn.strict_mode = true;
		new_fn.pc.emplace_back();
		{
			func_scope ns{new_fn};
			if (id != lex::token_lor) {
				auto endtk = id == '|' ? '|' : ')';
				if (!ns.lex().opt(endtk)) {
					while (true) {
						if (ns.lex().opt(lex::token_dots)) {
							if (new_fn.strict_mode) {
								ns.lex().error("strict variadic parameters require a named typed rest parameter");
								return {};
							}
							new_fn.is_vararg = true;
							if (ns.lex().check(endtk) == lex::token_error)
								return {};
							break;
						}

						auto arg_name = ns.lex().check(lex::token_name);
						if (arg_name == lex::token_error)
							return {};
						if (ns.lex().opt(lex::token_dots)) {
							if (new_fn.strict_mode) {
								ns.lex().error("strict rest parameter '%s' requires an element type", arg_name.str_val->c_str());
								return {};
							}
							new_fn.is_vararg = true;
							new_fn.set_vararg_name(arg_name.str_val);
							if (ns.lex().check(endtk) == lex::token_error)
								return {};
							break;
						}

						const bool optional = ns.lex().opt('?').has_value();
						const auto index    = new_fn.parameter_count++;
						if (new_fn.parameter_count > MAX_ARGS) {
							ns.lex().error("too many arguments.");
							return {};
						}

						strict::typespec parameter_type  = strict::typespec::any();
						bool             annotated       = false;
						string*          annotation_name = nullptr;
						if (ns.lex().opt(':')) {
							if (ns.lex().tok == lex::token_name)
								annotation_name = ns.lex().tok.str_val;
							auto annotation = parse_type_name(ns);
							if (!annotation)
								return {};
							parameter_type = std::move(*annotation);
							annotated      = true;
						} else if (new_fn.strict_mode) {
							auto automatic =
								 range::find_if(new_fn.type_aliases, [&](const type_alias_entry& alias) { return string_value_equals(alias.name, arg_name.str_val); });
							if (automatic == new_fn.type_aliases.end()) {
								ns.lex().error("strict parameter '%s' requires a type annotation", arg_name.str_val->c_str());
								return {};
							}
							parameter_type = automatic->value;
							annotated      = true;
						}
						if (ns.lex().opt(lex::token_dots)) {
							auto pack = annotation_name ? range::find_if(new_fn.variadic_type_aliases,
																		 [&](const variadic_type_alias_entry& alias) { return string_value_equals(alias.name, annotation_name); })
																 : new_fn.variadic_type_aliases.end();
							if (pack == new_fn.variadic_type_aliases.end()) {
								ns.lex().error("'%s...' is not a bound variadic generic type pack", annotation_name ? annotation_name->c_str() : "type");
								return {};
							}
							if (optional) {
								ns.lex().error("variadic generic parameter '%s' cannot be optional", arg_name.str_val->c_str());
								return {};
							}
							--new_fn.parameter_count;
							new_fn.parameter_types.insert(new_fn.parameter_types.end(), pack->values.begin(), pack->values.end());
							new_fn.is_vararg = true;
							new_fn.set_vararg_name(arg_name.str_val);
							if (ns.lex().check(endtk) == lex::token_error)
								return {};
							break;
						}
						new_fn.parameter_types.push_back(parameter_type);

						if (optional) {
							++opt_count;
							auto r = ns.add_local(arg_name.str_val, false, parameter_type);
							expression(any(number(index))).to_reg(ns, r);
							ns.emit(bc::VAGET, r, r);
						} else {
							if (opt_count != 0) {
								ns.lex().error("cannot accept a required argument after an optional one.");
								return {};
							}
							rc::retain(arg_name.str_val);
							new_fn.args.push_back({arg_name.str_val, parameter_type});
						}

						if (annotated) {
							if (optional) {
								auto count    = ns.alloc_reg();
								auto argindex = ns.alloc_reg();
								auto supplied = ns.alloc_reg();
								ns.emit(bc::VACNT, count);
								ns.set_reg(argindex, any(number(index)));
								ns.emit(bc::CGT, supplied, count, argindex);
								auto omitted = ns.emit(bc::JNS, 0, supplied);
								if (!type_check_var(ns, arg_name.str_val, parameter_type))
									return {};
								ns.jump_here(omitted);
								ns.free_reg(supplied);
								ns.free_reg(argindex);
								ns.free_reg(count);
							} else if (!type_check_var(ns, arg_name.str_val, parameter_type)) {
								return {};
							}
						}
						if (new_fn.strict_mode && parameter_type.kind == strict::type_kind::struct_ref) {
							if (optional) {
								auto& local  = ns.locals.back();
								auto  is_nil = ns.alloc_reg();
								ns.set_reg(is_nil, nil);
								ns.emit(bc::CEQ, is_nil, local.reg, is_nil);
								auto omitted = ns.emit(bc::JS, 0, is_nil);
								auto copied  = copy_struct_value(ns, expression(local.reg, false, parameter_type), parameter_type);
								if (copied.kind == expr::err)
									return {};
								copied.to_reg(ns, local.reg);
								ns.jump_here(omitted);
								ns.discard_regs(is_nil);
							} else {
								auto copied = copy_struct_value(ns, expression(int32_t(-FRAME_SIZE - new_fn.args.size()), false, parameter_type), parameter_type);
								if (copied.kind == expr::err)
									return {};
								auto local = ns.add_local(arg_name.str_val, false, parameter_type);
								copied.to_reg(ns, local);
							}
						}

						if (ns.lex().opt(endtk))
							break;
						if (ns.lex().check(',') == lex::token_error)
							return {};
					}
				}
			}
			new_fn.pc.front().o = bc::VACHK;
			new_fn.pc.front().a = msize_t(new_fn.args.size());
			if (shape) {
				shape->parameter_count = new_fn.parameter_count;
				shape->required_count  = new_fn.args.size();
				shape->is_vararg       = new_fn.is_vararg;
			}
			const auto minimum_arity  = static_cast<unsigned long long>(new_fn.args.size());
			auto*      argument_error = new_fn.own(string::format(ns.fn.L, "expected at least %llu argument%s", minimum_arity, minimum_arity == 1 ? "" : "s"));
			new_fn.pc.front().set_xmm(ns.add_const(argument_error).second.value);

			if (ns.lex().opt(lex::token_icall)) {
				auto annotation = parse_type_name(ns);
				if (!annotation)
					return {};
				new_fn.has_return_guard = true;
				new_fn.return_spec      = *annotation;
				auto guard_type         = *annotation;
				if (guard_type.kind == strict::type_kind::optional && !guard_type.children.empty()) {
					new_fn.return_nullable = true;
					guard_type             = guard_type.children.front();
				}
				if (guard_type.kind == strict::type_kind::self_ref) {
					new_fn.return_class_identity = ns.fn.class_decl_identity;
				} else if ((guard_type.kind == strict::type_kind::class_ref || guard_type.kind == strict::type_kind::struct_ref) && guard_type.declared) {
					new_fn.return_class = guard_type.declared;
				} else if (guard_type.kind == strict::type_kind::any && (guard_type.generic_index & runtime_type_tag)) {
					// Resolved at each return site in dynamic mode.
				} else {
					new_fn.return_type = guard_type.kind == strict::type_kind::any && (guard_type.generic_index & legacy_type_tag)
													 ? value_type(guard_type.generic_index & ~legacy_type_tag)
													 : strict::dynamic_guard(guard_type);
					if (new_fn.return_type == type_invalid) {
						ns.lex().error("return type '%s' cannot cross the dynamic boundary", strict::to_string(guard_type).c_str());
						return {};
					}
				}
			}

			ns.first_scope = true;
			expression e   = expr_parse(ns);
			if (e.kind == expr::err)
				return {};
			if (!new_fn.has_return_guard) {
				if (new_fn.strict_mode && e.static_type.kind == strict::type_kind::any) {
					ns.lex().error("strict function return type is unresolved; add an annotation or explicit conversion");
					return {};
				}
				new_fn.return_spec = new_fn.inferred_return ? *new_fn.inferred_return : e.static_type;
			}
			const bool body_terminated = !new_fn.pc.empty() && new_fn.pc.back().o == bc::RET;
			if (!body_terminated) {
				e = type_check_return(ns, e);
				if (e.kind == expr::err)
					return {};
				e.to_reg(ns, 0);
			}
		}

		function* result = write_func(new_fn, line_num, bc::reg(0));
		if (!result)
			return {};
		scope.fn.own(result);

		strict::typespec callable_type = strict::typespec::function();
		if (new_fn.strict_mode) {
			std::vector<strict::typespec> callable_signature = new_fn.parameter_types;
			callable_signature.push_back(new_fn.return_spec);
			callable_type = strict::typespec::function(std::move(callable_signature));
		}
		if (result->num_uval == 0)
			return expression(any(result), std::move(callable_type));

		auto uv = scope.alloc_reg((msize_t) new_fn.uvalues.size());
		for (size_t n = 0; n != new_fn.uvalues.size(); n++)
			expr_var(scope, new_fn.uvalues[n].id).to_reg(scope, uv + (bc::reg) n);
		scope.emit(bc::FDUP, uv, scope.add_const(result).first, uv);
		if (new_fn.uvalues.size() > 1)
			scope.free_reg(uv + 1, (msize_t) new_fn.uvalues.size() - 1);
		return expression(uv, false, std::move(callable_type));
	}

	// Parses a class declaration, returns the class value.
	//
	static expression parse_class(func_scope& scope, string* cl_name, bool value_semantics, table* attributes, uint64_t reserved_identity) {
		if (!cl_name)
			cl_name = scope.fn.own(string::format(scope.fn.L, "<anon-class-line-%u>", scope.lex().line));

		vclass* base = nullptr;
		if (scope.lex().opt(':')) {
			if (value_semantics) {
				scope.lex().error("struct declarations cannot inherit a base");
				return {};
			}
			auto name = expr_primary(scope, true);
			if (name.kind == expr::err)
				return {};
			if (name.kind != expr::imm || !name.imm.is_vcl()) {
				scope.lex().error("expected constant class value");
				return {};
			}
			base = name.imm.as_vcl();
			if (base->value_semantics) {
				scope.lex().error("class declarations cannot inherit from a struct");
				return {};
			}
		}

		if (scope.lex().check('{') == lex::token_error)
			return {};
		const auto                        ctor_line_beg  = scope.lex().line;
		const auto                        class_identity = reserved_identity ? reserved_identity : reserve_class_identity(scope.fn.L);
		std::vector<strict_field_binding> class_field_types;

		struct class_context_guard {
			func_state&                        fn;
			string*                            previous_name;
			uint64_t                           previous_identity;
			bool                               previous_strict;
			std::vector<strict_field_binding>* previous_fields;

			class_context_guard(func_state& fn, string* name, uint64_t identity, bool strict_members, std::vector<strict_field_binding>* fields)
				 : fn(fn),
					previous_name(fn.class_decl_name),
					previous_identity(fn.class_decl_identity),
					previous_strict(fn.class_strict_mode),
					previous_fields(fn.class_field_types) {
				fn.class_decl_name     = name;
				fn.class_decl_identity = identity;
				fn.class_strict_mode   = strict_members;
				fn.class_field_types   = fields;
			}
			~class_context_guard() {
				fn.class_decl_name     = previous_name;
				fn.class_decl_identity = previous_identity;
				fn.class_strict_mode   = previous_strict;
				fn.class_field_types   = previous_fields;
			}
		} class_context{scope.fn, cl_name, class_identity, scope.fn.strict_mode || has_attribute(scope.fn.L, attributes, "strict"), &class_field_types};

		msize_t                 obj_iterator    = 0;
		msize_t                 static_iterator = 0;
		std::vector<uint8_t>    obj_data        = {};
		std::vector<uint8_t>    static_data     = {};
		std::vector<field_pair> fields          = {};
		if (base) {
			obj_iterator    = base->object_length;
			static_iterator = base->static_length;
			obj_data        = {base->default_space(), base->default_space() + obj_iterator};
			static_data     = {base->static_space(), base->static_space() + static_iterator};
			fields          = {base->fields().begin(), base->fields().end()};
		}

		struct parsed_special {
			trait     which;
			function* method;
		};
		std::vector<parsed_special>      specials;
		std::vector<property_definition> properties;
		std::vector<string*>             declared_values;
		uint64_t                         declared_traits  = 0;
		uint64_t                         dynamic_traits   = 0;
		function*                        constructor_body = nullptr;
		int32_t                          next_union_group = 0;
		std::vector<bool>                static_member_branches;

		auto report_runtime_error = [&](const char* fallback) {
			if (scope.fn.L->last_ex.is_str())
				scope.lex().error("%s", scope.fn.L->last_ex.as_str()->c_str());
			else
				scope.lex().error("%s", fallback);
		};
		auto same_name         = [](string* lhs, string* rhs) { return string_value_equals(lhs, rhs); };
		auto find_field        = [&](string* name) { return range::find_if(fields, [&](const field_pair& field) { return same_name(field.key, name); }); };
		auto property_declared = [&](string* name) {
			return range::find_if(properties, [&](const property_definition& property) { return same_name(property.name, name); }) != properties.end();
		};
		auto reserve_value_name = [&](string* name) {
			if (range::find_if(declared_values, [&](string* previous) { return same_name(previous, name); }) != declared_values.end() || property_declared(name)) {
				scope.lex().error("duplicate class member '%s'", name->c_str());
				return false;
			}
			declared_values.push_back(name);
			return true;
		};
		auto finish_member = [&]() {
			if (scope.lex().tok == ',') {
				scope.lex().error("class members must be separated by a semicolon or newline, not ','");
				return false;
			}
			if (scope.lex().opt(';')) {
				while (scope.lex().opt(';')) {
				}
				return true;
			}
			if (scope.lex().tok != '}' && scope.lex().tok.source_line <= scope.lex().last_token_line) {
				scope.lex().error("expected ';' or newline between class members");
				return false;
			}
			return true;
		};
		auto parse_member_body = [&](string* member_name, function_shape* shape) -> function* {
			if (scope.lex().tok != '(') {
				scope.lex().check('(');
				return nullptr;
			}
			auto fn = parse_function(scope, member_name, shape);
			if (fn.kind == expr::err)
				return nullptr;
			if (fn.kind != expr::imm || !fn.imm.is_fn()) {
				scope.lex().error("class member functions cannot capture runtime values");
				return nullptr;
			}
			return fn.imm.as_fn();
		};
		auto exact_arity = [&](const function_shape& shape, msize_t expected, std::string_view description) {
			if (shape.parameter_count == expected && shape.required_count == expected && !shape.is_vararg)
				return true;
			scope.lex().error("%.*s expects exactly %u parameter%s", int(description.size()), description.data(), unsigned(expected), expected == 1 ? "" : "s");
			return false;
		};
		auto inherited_trait_is_dynamic = [&](trait which) {
			const uint64_t bit = uint64_t{1} << size_t(which);
			for (vclass* current = base; current; current = current->super) {
				if (current->traits && current->traits->methods[size_t(which)])
					return (current->dynamic_traits & bit) != 0;
			}
			return true;
		};

		bool       has_own_initializer = false;
		func_state init_fn{scope.fn, scope};
		{
			func_scope cscope{init_fn};
			const auto self = bc::reg(FRAME_SELF);
			auto       tmpk = cscope.alloc_reg();
			auto       tmpv = cscope.alloc_reg();

			if (base && base->initializer) {
				expression(self, true).push(cscope);
				expression(any(base->initializer)).push(cscope);
				cscope.emit(bc::CALL, tmpv, 0);
			}

			while (true) {
				while (scope.lex().opt(';')) {
				}
				if (scope.lex().tok == '}') {
					scope.lex().next();
					if (static_member_branches.empty())
						break;
					const bool selected_true = static_member_branches.back();
					static_member_branches.pop_back();
					if (scope.lex().opt(lex::token_else)) {
						if (selected_true) {
							if (!skip_static_block(scope))
								return {};
						} else {
							if (scope.lex().check('{') == lex::token_error)
								return {};
							static_member_branches.push_back(false);
						}
					}
					continue;
				}

				const bool dynamic = scope.lex().opt(lex::token_dyn).has_value();
				auto       first   = scope.lex().check(lex::token_name);
				if (first == lex::token_error)
					return {};
				auto spelling     = first.str_val->view();
				bool atomic_field = false;
				if (spelling == "atomic" && scope.lex().tok == lex::token_name) {
					atomic_field = true;
					if (!(scope.fn.class_strict_mode || scope.fn.strict_mode)) {
						scope.lex().error("atomic fields require strict mode");
						return {};
					}
					first = scope.lex().check(lex::token_name);
					if (first == lex::token_error)
						return {};
					spelling = first.str_val->view();
					if (spelling == "union") {
						scope.lex().error("atomic fields require strict mode");
						return {};
					}
				}

				if (spelling == "static") {
					if (!(scope.fn.class_strict_mode || scope.fn.strict_mode)) {
						scope.lex().error("static evaluation requires strict mode");
						return {};
					}
					auto operation = scope.lex().next();
					if (operation.id != lex::token_if) {
						scope.lex().error_at(operation, "only static if is valid in a class body");
						return {};
					}
					auto condition = expr_parse(cscope);
					if (condition.kind == expr::err)
						return {};
					auto selected = static_boolean(cscope, condition, operation);
					if (!selected)
						return {};
					if (*selected) {
						if (scope.lex().check('{') == lex::token_error)
							return {};
						static_member_branches.push_back(true);
					} else {
						if (!skip_static_block(scope))
							return {};
						if (scope.lex().opt(lex::token_else)) {
							if (scope.lex().check('{') == lex::token_error)
								return {};
							static_member_branches.push_back(false);
						}
					}
					continue;
				}

				if (spelling == "union" && scope.lex().tok == '{') {
					if (!value_semantics || !(scope.fn.class_strict_mode || scope.fn.strict_mode)) {
						scope.lex().error("union is only valid inside strict structs");
						return {};
					}
					scope.lex().next();
					const int32_t       group = next_union_group++;
					std::vector<size_t> union_fields;
					msize_t             union_size  = 0;
					msize_t             union_align = 1;
					while (!scope.lex().opt('}')) {
						while (scope.lex().opt(';')) {
						}
						if (scope.lex().tok == lex::token_name && scope.lex().tok.str_val->view() == "atomic" && scope.lex().lookahead() == lex::token_name) {
							scope.lex().error("atomic fields require strict mode");
							return {};
						}
						if (scope.lex().tok == '}') {
							scope.lex().next();
							break;
						}
						if (scope.lex().tok == lex::token_name && scope.lex().tok.str_val->view() == "uninit")
							scope.lex().next();
						auto field_name = scope.lex().check(lex::token_name);
						if (field_name == lex::token_error || scope.lex().check(':') == lex::token_error)
							return {};
						if (!reserve_value_name(field_name.str_val) || find_field(field_name.str_val) != fields.end()) {
							scope.lex().error("duplicate class member '%s'", field_name.str_val->c_str());
							return {};
						}
						auto spec = parse_type_name(cscope);
						if (!spec)
							return {};
						if (spec->kind == strict::type_kind::view) {
							cscope.lex().error("view values may not be stored in fields");
							return {};
						}
						auto& field           = fields.emplace_back();
						field.key             = field_name.str_val;
						field.value.is_static = false;
						field.value.is_dyn    = false;
						field.value.ty        = strict::dynamic_storage(*spec);
						if (field.value.ty == type::any) {
							auto guard = strict::dynamic_guard(*spec);
							if (guard != type_invalid)
								field.value.ty = to_type(guard);
						}
						if ((spec->kind == strict::type_kind::class_ref || spec->kind == strict::type_kind::struct_ref) && spec->declared)
							field.value.class_identity = spec->declared->identity;
						else if (spec->kind == strict::type_kind::self_ref)
							field.value.class_identity = class_identity;
						class_field_types.push_back({field_name.str_val, *spec, group});
						union_fields.push_back(fields.size() - 1);
						union_size  = std::max(union_size, size_of_data(field.value.ty));
						union_align = std::max(union_align, align_of_data(field.value.ty));
						if (scope.lex().opt(';')) {
							while (scope.lex().opt(';')) {
							}
						} else if (scope.lex().tok != '}' && scope.lex().tok.source_line <= scope.lex().last_token_line) {
							scope.lex().error("expected ';' or newline between union fields");
							return {};
						}
					}
					const msize_t offset = (obj_iterator + union_align - 1) & ~(union_align - 1);
					LI_ASSERT(offset <= field_info::max_offset);
					for (size_t index : union_fields)
						fields[index].value.offset = offset;
					obj_iterator = offset + union_size;
					obj_data.resize(obj_iterator, 0);
					if (!finish_member())
						return {};
					continue;
				}

				bool explicit_uninit = false;
				if (spelling == "uninit") {
					explicit_uninit = true;
					first           = scope.lex().check(lex::token_name);
					if (first == lex::token_error)
						return {};
					spelling = first.str_val->view();
				}

				const bool accessor = (spelling == "get" || spelling == "set") && scope.lex().tok == lex::token_name;
				if (atomic_field && (accessor || scope.lex().tok == '!' || scope.lex().tok == '(' || scope.lex().tok == '<')) {
					scope.lex().error("atomic fields must be numeric");
					return {};
				}
				if (accessor) {
					const auto access = spelling == "get" ? property_access::get : property_access::set;
					auto       name   = scope.lex().next();
					if (find_field(name.str_val) != fields.end() ||
						 range::find_if(declared_values, [&](string* previous) { return same_name(previous, name.str_val); }) != declared_values.end()) {
						scope.lex().error("property '%s' shadows a field or method", name.str_val->c_str());
						return {};
					}
					if (range::find_if(properties, [&](const property_definition& property) {
							 return property.access == access && same_name(property.name, name.str_val);
						 }) != properties.end()) {
						scope.lex().error("duplicate %s accessor for property '%s'", spelling.data(), name.str_val->c_str());
						return {};
					}

					auto* member_name =
						 scope.fn.own(string::format(scope.fn.L, "class %s.%.*s %s", cl_name->c_str(), int(spelling.size()), spelling.data(), name.str_val->c_str()));
					function_shape shape;
					function*      method = parse_member_body(member_name, &shape);
					if (!method)
						return {};
					const msize_t expected = access == property_access::get ? 0 : 1;
					if (!exact_arity(shape, expected, access == property_access::get ? "property getter" : "property setter"))
						return {};
					properties.push_back({
						 .name    = name.str_val,
						 .method  = method,
						 .access  = access,
						 .dynamic = dynamic,
					});
					if (!finish_member())
						return {};
					continue;
				}

				if (scope.lex().opt('!')) {
					if (spelling == "to") {
						if (dynamic || scope.lex().tok != '<') {
							scope.lex().error("to! must be a non-dynamic generic conversion method");
							return {};
						}
						if (!reserve_value_name(first.str_val))
							return {};
						auto existing             = fields.emplace(fields.end());
						existing->key             = first.str_val;
						existing->value.is_static = true;
						LI_ASSERT(static_iterator <= field_info::max_offset);
						existing->value.offset = static_iterator;
						existing->value.ty     = type::fn;
						static_iterator += sizeof(function*);
						static_data.resize(static_iterator, 0);
						auto* member_name = scope.fn.own(string::format(scope.fn.L, "class %s.to!", cl_name->c_str()));
						auto  parsed      = parse_generic_declaration(scope, member_name, false, false, nullptr, true);
						if (parsed.kind != expr::imm || !parsed.imm.is_fn())
							return {};
						auto* method = parsed.imm.as_fn();
						class_field_types.push_back({first.str_val, function_static_type(method)});
						any(method).store_at(static_data.data() + existing->value.offset, type::fn);
						if (!finish_member())
							return {};
						continue;
					}
					if (spelling == "new") {
						if (dynamic) {
							scope.lex().error("constructor new! cannot be dynamic");
							return {};
						}
						if (constructor_body) {
							scope.lex().error("duplicate constructor new!");
							return {};
						}
						auto* member_name = scope.fn.own(string::format(scope.fn.L, "class %s.new!", cl_name->c_str()));
						constructor_body  = parse_member_body(member_name, nullptr);
						if (!constructor_body)
							return {};
						if (!finish_member())
							return {};
						continue;
					}

					if (spelling == "le") {
						scope.lex().error("le! is derived from lt! and eq! and cannot be declared");
						return {};
					}
					const trait which = resolve_trait_name(spelling);
					if (!is_trait_method(which)) {
						scope.lex().error("unknown special method '%s!'", first.str_val->c_str());
						return {};
					}
					const uint64_t bit             = uint64_t{1} << size_t(which);
					function*      previous_method = nullptr;
					if (declared_traits & bit) {
						auto previous   = range::find_if(specials, [&](const parsed_special& special) { return special.which == which; });
						previous_method = previous == specials.end() ? nullptr : previous->method;
						if (!previous_method || !previous_method->proto || !previous_method->proto->generic || scope.lex().tok != '<') {
							scope.lex().error("duplicate special method '%s!'", first.str_val->c_str());
							return {};
						}
					}
					if (base && has_trait(any(base), which) && !inherited_trait_is_dynamic(which)) {
						scope.lex().error("cannot override final inherited special method '%s!'", first.str_val->c_str());
						return {};
					}

					auto*          member_name = scope.fn.own(string::format(scope.fn.L, "class %s.%s!", cl_name->c_str(), first.str_val->c_str()));
					function_shape shape;
					const bool     generic_method = scope.lex().tok == '<' || ((scope.fn.class_strict_mode || scope.fn.strict_mode) && scope.lex().tok == '(' &&
																									  function_has_unannotated_parameter(scope.fn.lex));
					function*      method;
					if (generic_method) {
						auto parsed = parse_generic_declaration(scope, member_name, false, false, nullptr, scope.lex().tok == '<', previous_method);
						if (parsed.kind != expr::imm || !parsed.imm.is_fn())
							return {};
						method = parsed.imm.as_fn();
					} else {
						method = parse_member_body(member_name, &shape);
						if (!method)
							return {};
					}

					msize_t expected = 0;
					bool    fixed    = true;
					switch (which) {
						case trait::at:
						case trait::add:
						case trait::sub:
						case trait::mul:
						case trait::div:
						case trait::mod:
						case trait::pow:
						case trait::lt:
						case trait::eq:
							expected = 1;
							break;
						case trait::set:
							expected = 2;
							break;
						case trait::call:
							fixed = false;
							break;
						case trait::len:
						case trait::neg:
						case trait::str:
						case trait::del:
						case trait::next:
							break;
						default:
							scope.lex().error("invalid class special method '%s!'", first.str_val->c_str());
							return {};
					}
					if (!generic_method && fixed && !exact_arity(shape, expected, util::fmt("special method %s!", first.str_val->c_str())))
						return {};

					declared_traits |= bit;
					if (dynamic)
						dynamic_traits |= bit;
					if (!previous_method)
						specials.push_back({which, method});
					if (!finish_member())
						return {};
					continue;
				}

				if (scope.lex().tok == '(' || scope.lex().tok == '<') {
					if (!reserve_value_name(first.str_val))
						return {};
					auto existing = find_field(first.str_val);
					if (existing == fields.end()) {
						existing                  = fields.emplace(fields.end());
						existing->key             = first.str_val;
						existing->value.is_static = true;
						LI_ASSERT(static_iterator <= field_info::max_offset);
						existing->value.offset = static_iterator;
						existing->value.ty     = type::fn;
						static_iterator += sizeof(function*);
						static_data.resize(static_iterator, 0);
					} else if (!existing->value.is_static || !existing->value.is_dyn || existing->value.ty != type::fn) {
						scope.lex().error("cannot override final inherited member '%s'", first.str_val->c_str());
						return {};
					}
					if (base && class_has_property(base, first.str_val)) {
						scope.lex().error("method '%s' shadows an inherited property", first.str_val->c_str());
						return {};
					}

					auto*      member_name = scope.fn.own(string::format(scope.fn.L, "class %s.%s", cl_name->c_str(), first.str_val->c_str()));
					const bool generic_method =
						 scope.lex().tok == '<' || ((scope.fn.class_strict_mode || scope.fn.strict_mode) && function_has_unannotated_parameter(scope.fn.lex));
					function* method;
					if (generic_method) {
						auto parsed = parse_generic_declaration(scope, member_name, false, false, nullptr, scope.lex().tok == '<');
						if (parsed.kind != expr::imm || !parsed.imm.is_fn())
							return {};
						method = parsed.imm.as_fn();
					} else {
						method = parse_member_body(member_name, nullptr);
						if (!method)
							return {};
					}
					class_field_types.push_back({first.str_val, function_static_type(method)});
					existing->value.is_dyn = dynamic;
					any(method).store_at(static_data.data() + existing->value.offset, type::fn);
					if (!finish_member())
						return {};
					continue;
				}

				if (dynamic) {
					scope.lex().error("dyn is only valid on methods, properties, and special methods");
					return {};
				}
				if (!reserve_value_name(first.str_val))
					return {};
				if (find_field(first.str_val) != fields.end()) {
					scope.lex().error("definition shadows the previous value of field %s", first.str_val->c_str());
					return {};
				}
				if (base && class_has_property(base, first.str_val)) {
					scope.lex().error("field '%s' shadows an inherited property", first.str_val->c_str());
					return {};
				}

				auto&                           field       = fields.emplace_back();
				uint64_t                        field_initv = 0;
				vclass*                         field_class = nullptr;
				std::optional<strict::typespec> field_spec;
				field.key             = first.str_val;
				field.value.is_dyn    = false;
				field.value.is_static = false;
				field.value.is_atomic = atomic_field;
				field.value.ty        = type::any;

				if (cscope.lex().opt(':')) {
					field_spec = parse_type_name(cscope);
					if (!field_spec)
						return {};
					if (field_spec->kind == strict::type_kind::view) {
						cscope.lex().error("view values may not be stored in fields");
						return {};
					}
					if (atomic_field && !is_numeric_type(*field_spec)) {
						cscope.lex().error("atomic fields must be numeric");
						return {};
					}
					field.value.ty = field_spec->kind == strict::type_kind::any && (field_spec->generic_index & legacy_type_tag)
												? to_type(value_type(field_spec->generic_index & ~legacy_type_tag))
												: strict::dynamic_storage(*field_spec);
					if (field.value.ty == type::any) {
						auto guard = strict::dynamic_guard(*field_spec);
						if (guard != type_invalid)
							field.value.ty = to_type(guard);
					}
					if (field_spec->kind == strict::type_kind::class_ref || field_spec->kind == strict::type_kind::struct_ref) {
						field_class                = field_spec->declared;
						field.value.class_identity = field_class ? field_class->identity : 0;
					} else if (field_spec->kind == strict::type_kind::self_ref) {
						field.value.class_identity = class_identity;
					}
				} else if (scope.fn.class_strict_mode || value_semantics) {
					cscope.lex().error("%s field '%s' requires a type annotation", value_semantics ? "struct" : "strict", first.str_val->c_str());
					return {};
				}
				if (field_spec)
					class_field_types.push_back({first.str_val, *field_spec, -1, atomic_field});

				if (cscope.lex().opt('=')) {
					expression value;
					const bool typed_default_kind =
						 field_spec && (field_spec->kind == strict::type_kind::boolean || field_spec->kind == strict::type_kind::number ||
												 field_spec->kind == strict::type_kind::sized_int || field_spec->kind == strict::type_kind::f32 ||
												 field_spec->kind == strict::type_kind::f64 || field_spec->kind == strict::type_kind::string ||
												 field_spec->kind == strict::type_kind::optional || field_spec->kind == strict::type_kind::class_ref ||
												 field_spec->kind == strict::type_kind::struct_ref);
					const bool strict_braced_initializer = field_spec && cscope.fn.strict_mode && cscope.lex().tok == '{';
					const bool strict_empty_initializer  = typed_default_kind && strict_braced_initializer && cscope.lex().lookahead() == '}';
					if (strict_empty_initializer) {
						cscope.lex().next();
						cscope.lex().next();
						auto initialized = strict_default_initializer(cscope, *field_spec);
						if (!initialized) {
							cscope.lex().error("type '%s' has no strict default initializer", strict::to_string(*field_spec).c_str());
							return {};
						}
						value = std::move(*initialized);
					} else {
						value = expr_parse(cscope);
						if (value.kind == expr::err)
							return {};
						if (strict_braced_initializer && is_numeric_type(*field_spec) && is_numeric_type(value.static_type))
							value.static_type = *field_spec;
					}
					if (field_spec && !strict_assignable(cscope, *field_spec, value, util::fmt("field '%s'", first.str_val->c_str())))
						return {};
					if (!cscope.fn.strict_mode && field_spec && value.static_type.kind != strict::type_kind::any &&
						 !strict::assignable(*field_spec, value.static_type)) {
						cscope.lex().error("cannot initialize field of type (%s) with value of type (%s).", strict::to_string(*field_spec).c_str(),
							 strict::to_string(value.static_type).c_str());
						return {};
					}
					if (value.kind == expr::imm) {
						if (!field_spec && field.value.ty != type::any && value.imm.xtype() != field.value.ty) {
							auto expected = get_type_name(cscope.fn.L, field.value.ty);
							cscope.lex().error(
								 "cannot initialize field of type (%.*s) with value of type (%s).", expected.size(), expected.data(), value.imm.type_name());
							return {};
						}
						value.imm.store_at(&field_initv, field.value.ty);
					} else {
						has_own_initializer = true;
						cscope.set_reg(tmpk, field.key);
						cscope.emit(bc::SSET, tmpk, value.to_anyreg(cscope), self);
					}
				} else if (explicit_uninit) {
					// Deliberately left with zeroed backing bytes; reads are controlled by strict flow checks for unions.
				} else if (field.value.ty == type::arr) {
					has_own_initializer = true;
					cscope.set_reg(tmpk, field.key);
					cscope.emit(bc::ANEW, tmpv, 0);
					cscope.emit(bc::SSET, tmpk, tmpv, self);
				} else if (field.value.ty == type::tbl) {
					has_own_initializer = true;
					cscope.set_reg(tmpk, field.key);
					cscope.emit(bc::TNEW, tmpv, 0);
					cscope.emit(bc::SSET, tmpk, tmpv, self);
				} else if (field_class) {
					has_own_initializer = true;
					expression(nil).push(cscope);
					expression(any(field_class)).push(cscope);
					cscope.emit(bc::CALL, tmpv, 0);
					cscope.set_reg(tmpk, field.key);
					cscope.emit(bc::SSET, tmpk, tmpv, self);
				} else if (field.value.ty == type::tarr) {
					cscope.lex().error("typed-array fields require an initial value");
					return {};
				} else if (field.value.ty == type::obj || field.value.ty == type::vcl || field.value.ty == type::weak) {
					auto expected = get_type_name(cscope.fn.L, field.value.ty);
					cscope.lex().error("cannot default initialize field of type %.*s.", expected.size(), expected.data());
					return {};
				} else {
					any default_value = any::make_default(cscope.fn.L, field.value.ty);
					default_value.store_at(&field_initv, field.value.ty);
				}

				const msize_t size        = size_of_data(field.value.ty);
				const msize_t align       = align_of_data(field.value.ty);
				const msize_t next_offset = ((obj_iterator + align - 1) & ~(align - 1)) + size;
				LI_ASSERT(next_offset - size <= field_info::max_offset);
				field.value.offset = next_offset - size;
				obj_iterator       = next_offset;
				obj_data.resize(obj_iterator, 0);
				memcpy(obj_data.data() + field.value.offset, &field_initv, size);

				if (!finish_member())
					return {};
			}

			cscope.emit(bc::RET, self);
		}

		function* initializer = base ? base->initializer : nullptr;
		if (has_own_initializer) {
			auto* init_name = scope.fn.own(string::format(scope.fn.L, "class %s implicit initializer", cl_name->c_str()));
			init_fn.set_decl_name(init_name);
			initializer = write_func(init_fn, ctor_line_beg, FRAME_SELF);
			if (!initializer)
				return {};
			if (initializer->num_uval != 0) {
				scope.lex().error("class field initializers cannot capture runtime values");
				rc::release(scope.fn.L, initializer);
				return {};
			}
			scope.fn.own(initializer);
			initializer->proto->attr |= func_attr_inline;
		}

		auto* vcl = scope.fn.own(vclass::create(scope.fn.L, cl_name, fields, obj_data, static_data, base, class_identity, value_semantics));
		if (attributes) {
			vcl->attributes = attributes;
			rc::retain(vcl->attributes);
		}
		scope.fn.nominal_types.push_back({vcl, class_field_types});
		if (scope.fn.type_table && scope.fn.type_table->contains(any(cl_name)) && !scope.fn.type_table->set(scope.fn.L, any(cl_name), any(vcl))) {
			report_runtime_error("failed to register nominal type");
			return {};
		}
		LI_ASSERT(vcl->object_length == obj_iterator);
		LI_ASSERT(vcl->static_length == static_iterator);
		if (!vcl->set_initializer(scope.fn.L, initializer)) {
			report_runtime_error("failed to install class initializer");
			return {};
		}

		for (const parsed_special& special : specials) {
			any result = set_trait(scope.fn.L, any(vcl), special.which, any(special.method));
			if (result.is_exc()) {
				report_runtime_error("failed to install class special method");
				return {};
			}
			rc::release(scope.fn.L, result);
		}
		vcl->dynamic_traits = dynamic_traits;

		for (const property_definition& property : properties) {
			if (!vcl->define_property(scope.fn.L, property)) {
				report_runtime_error("failed to define class property");
				return {};
			}
		}

		if (constructor_body) {
			if (!vcl->set_constructor_body(scope.fn.L, constructor_body)) {
				report_runtime_error("failed to install class constructor");
				return {};
			}
		} else {
			func_state ctor_fn{scope.fn, scope};
			auto*      ctor_name = scope.fn.own(string::format(scope.fn.L, "class %s implicit ctor", cl_name->c_str()));
			ctor_fn.set_decl_name(ctor_name);
			{
				func_scope cscope{ctor_fn};
				auto       self = cscope.alloc_reg();
				cscope.emit(bc::STRIV, self, 0);

				auto tmpk = cscope.alloc_reg();
				auto tmpv = cscope.alloc_reg();
				if (initializer) {
					expression(self, true).push(cscope);
					expression(any(initializer)).push(cscope);
					cscope.emit(bc::CALL, tmpv, 0);
				}

				cscope.emit(bc::VACNT, tmpk);
				cscope.set_reg(tmpv, number(0));
				cscope.emit(bc::CEQ, tmpk, tmpk, tmpv);
				auto no_arguments = cscope.emit(bc::JS, 0, tmpk);

				auto argument = cscope.alloc_reg();
				cscope.emit(bc::VAGET, argument, tmpv);
				cscope.emit(bc::CTY, tmpk, argument, type_table);
				cscope.throw_if_not(tmpk, "implicit ctor must be passed nil or a valid table.");

				auto compare = cscope.alloc_reg();
				auto nil_reg = cscope.alloc_reg();
				cscope.set_reg(nil_reg, nil);
				for (const field_pair& field : fields) {
					if (field.value.is_static)
						continue;
					cscope.set_reg(tmpk, field.key);
					cscope.emit(bc::TGETR, tmpv, tmpk, argument);
					cscope.emit(bc::CEQ, compare, nil_reg, tmpv);
					auto missing = cscope.emit(bc::JS, 0, compare);
					cscope.emit(bc::SSET, tmpk, tmpv, self);
					cscope.jump_here(missing);
				}

				cscope.jump_here(no_arguments);
				cscope.emit(bc::RET, self);
			}

			function* ctor = write_func(ctor_fn, ctor_line_beg, 0);
			if (!ctor)
				return {};
			ctor->proto->attr |= func_attr_inline;
			if (!vcl->set_ctor(scope.fn.L, ctor)) {
				rc::release(scope.fn.L, ctor);
				report_runtime_error("failed to install implicit class constructor");
				return {};
			}
			rc::release(scope.fn.L, ctor);
		}
		return any(vcl);
	}

	// Parses block-like constructs, returns the result.
	//
	static expression parse_if(func_scope& scope) {
		// Fetch the conditional expression, dispatch to ToS.
		//
		auto cc = expr_parse(scope);
		if (cc.kind == expr::err)
			return {};
		if (scope.fn.strict_mode && cc.static_type.kind != strict::type_kind::boolean) {
			scope.lex().error("if condition must be bool, got '%s'", strict::to_string(cc.static_type).c_str());
			return {};
		}
		cc = expression(cc.to_nextreg(scope), false, cc.static_type);

		// Define block reader.
		//
		auto block_or_exp = [&]() -> std::optional<strict::typespec> {
			expression value;
			if (scope.lex().opt('{'))
				value = expr_block(scope, cc.reg);
			else {
				value = expr_parse(scope);
				if (value.kind != expr::err)
					value.to_reg(scope, cc.reg);
			}
			if (value.kind == expr::err)
				return std::nullopt;
			return value.static_type;
		};

		// Emit the placeholder JCC.
		//
		auto jcc_pos = scope.emit(bc::JNS, 0, cc.reg);

		// Schedule the if block.
		//
		auto true_type = block_or_exp();
		if (!true_type)
			return {};

		// Emit the escape jump, fix the JCC.
		//
		auto jmp_pos = scope.emit(bc::JMP);
		scope.jump_here(jcc_pos);

		// If there is an else:
		//
		strict::typespec false_type = strict::typespec::nil();
		if (scope.lex().opt(lex::token_else)) {
			auto parsed = block_or_exp();
			if (!parsed)
				return {};
			false_type = std::move(*parsed);
		}
		// Otherwise simply null the target.
		//
		else {
			scope.set_reg(cc.reg, nil);
		}

		// Fix the escape.
		//
		scope.jump_here(jmp_pos);

		// Yield the register.
		//
		strict::typespec result_type = strict::typespec::any();
		if (scope.fn.strict_mode) {
			if (strict::assignable(*true_type, false_type))
				result_type = *true_type;
			else if (strict::assignable(false_type, *true_type))
				result_type = false_type;
			else {
				scope.lex().error("incompatible if branch types '%s' and '%s'", strict::to_string(*true_type).c_str(), strict::to_string(false_type).c_str());
				return {};
			}
		}
		return expression(cc.reg, false, std::move(result_type));
	}
	static expression parse_match(func_scope& scope) {
		auto subject = expr_parse(scope);
		if (subject.kind == expr::err || scope.lex().check('{') == lex::token_error) {
			return {};
		}
		subject = subject.to_nextreg(scope);

		expression result = expression(nil).to_nextreg(scope);
		auto       end    = scope.make_label();

		while (!scope.lex().opt('}')) {
			auto       matched  = scope.make_label();
			auto       next_arm = scope.make_label();
			func_scope arm{scope.fn};

			// A guarded identifier binds the subject for the guard and body.
			// Other identifiers remain ordinary value/class patterns so that
			// alternatives such as `red | green` retain their natural meaning.
			//
			string* capture       = nullptr;
			bool    unconditional = false;
			if (arm.lex().tok == lex::token_name) {
				auto name = arm.lex().tok.str_val;
				if (name->view() == "_") {
					arm.lex().next();
					unconditional = true;
				} else if (arm.lex().lookahead() == lex::token_if) {
					capture       = arm.lex().next().str_val;
					unconditional = true;
				}
			}
			if (capture) {
				arm.add_local_at(capture, true, subject.reg);
			}

			while (!unconditional) {
				std::optional<value_type> pattern_type;
				switch (arm.lex().tok.id) {
					case lex::token_bool:
						pattern_type = type_bool;
						break;
					case lex::token_number:
						pattern_type = type_number;
						break;
					case lex::token_table:
						pattern_type = type_table;
						break;
					case lex::token_array:
						pattern_type = type_array;
						break;
					case lex::token_string:
						pattern_type = type_string;
						break;
					case lex::token_object:
						pattern_type = type_object;
						break;
					case lex::token_class:
						pattern_type = type_class;
						break;
					case lex::token_function:
						pattern_type = type_function;
						break;
					default:
						break;
				}

				expression condition;
				if (pattern_type) {
					arm.lex().next();
					auto cc = arm.alloc_reg();
					arm.emit(bc::CTY, cc, subject.reg, *pattern_type);
					condition = cc;
				} else {
					auto lower = expr_parse_core(arm);
					if (lower.kind == expr::err) {
						return {};
					}

					if (arm.lex().tok == lex::token_range || arm.lex().tok == lex::token_rangei) {
						bool inclusive = arm.lex().next() == lex::token_rangei;
						auto upper     = expr_parse_core(arm);
						if (upper.kind == expr::err) {
							return {};
						}
						auto above_lower = emit_binop(arm, lower, bc::CLE, subject);
						auto below_upper = emit_binop(arm, subject, inclusive ? bc::CLE : bc::CLT, upper);
						condition        = emit_binop(arm, above_lower, bc::LAND, below_upper);
					} else if (lower.kind == expr::imm) {
						if (lower.imm.is_vcl()) {
							auto cl = lower.to_anyreg(arm);
							auto cc = arm.alloc_reg();
							arm.emit(bc::CTYX, cc, subject.reg, cl);
							condition = cc;
						} else {
							condition = emit_binop(arm, subject, bc::CEQ, lower);
						}
					} else {
						// Runtime values that are classes use `is`; all other
						// dynamic values use equality.
						//
						auto pattern  = lower.to_anyreg(arm);
						auto is_class = arm.alloc_reg();
						auto cc       = arm.alloc_reg();
						arm.emit(bc::CTY, is_class, pattern, type_class);
						auto compare_value = arm.emit(bc::JNS, 0, is_class);
						arm.emit(bc::CTYX, cc, subject.reg, pattern);
						auto compared = arm.emit(bc::JMP);
						arm.jump_here(compare_value);
						arm.emit(bc::CEQ, cc, subject.reg, pattern);
						arm.jump_here(compared);
						condition = cc;
					}
				}

				arm.emit(bc::JS, matched, condition.to_anyreg(arm));
				if (!arm.lex().opt('|')) {
					break;
				}
				if (arm.lex().tok == lex::token_name && arm.lex().tok.str_val->view() == "_") {
					arm.lex().next();
					unconditional = true;
					break;
				}
			}

			if (!unconditional) {
				arm.emit(bc::JMP, next_arm);
			}
			arm.set_label_here(matched);

			if (arm.lex().opt(lex::token_if)) {
				auto guard = expr_parse(arm);
				if (guard.kind == expr::err) {
					return {};
				}
				arm.emit(bc::JNS, next_arm, guard.to_anyreg(arm));
			}
			if (arm.lex().check(lex::token_fatarrow) == lex::token_error) {
				return {};
			}

			auto value = expr_parse(arm);
			if (value.kind == expr::err) {
				return {};
			}
			value.to_reg(arm, result.reg);
			arm.emit(bc::JMP, end);
			arm.set_label_here(next_arm);

			if (!arm.lex().opt(',') && !arm.lex().opt(';') && arm.lex().tok != '}') {
				arm.lex().error("expected ',' or '}' after match arm");
				return {};
			}
		}

		scope.set_label_here(end);
		return result;
	}
	static bool parse_loop_else(func_scope& scope, bc::reg result) {
		if (!scope.lex().opt(lex::token_else)) {
			return true;
		}
		if (scope.lex().check('{') == lex::token_error) {
			return false;
		}
		return expr_block(scope, result).kind != expr::err;
	}
	static expression parse_loop(func_scope& scope) {
		// Allocate new continue and break labels.
		//
		auto pb = std::exchange(scope.lbl_break, scope.make_label());
		auto pc = std::exchange(scope.lbl_continue, scope.make_label());
		auto pr = scope.reg_break;
		auto ob = std::exchange(scope.owner_break, &scope);
		auto oc = std::exchange(scope.owner_continue, &scope);

		// Point the continue label at the beginning.
		//
		scope.set_label_here(scope.lbl_continue);

		// Reserve next register for break-with-value, initialize to nil.
		//
		expression result = expression{nil}.to_nextreg(scope);
		scope.reg_break   = result.reg;

		// Parse the block.
		//
		if (scope.lex().check('{') == lex::token_error) {
			return {};
		}
		auto body_base = scope.reg_next;
		if (expr_block(scope).kind == expr::err) {
			return {};
		}
		scope.discard_regs(body_base);

		// Jump to continue.
		//
		scope.emit(bc::JMP, scope.lbl_continue);

		// Emit break label.
		//
		scope.set_label_here(scope.lbl_break);

		// Restore the labels.
		//
		scope.lbl_break      = pb;
		scope.lbl_continue   = pc;
		scope.reg_break      = pr;
		scope.owner_break    = ob;
		scope.owner_continue = oc;

		// Return the result.
		//
		return result;
	}
	static expression parse_while(func_scope& scope) {
		// A condition-false exit reaches the else path; break skips it.
		//
		auto pb              = scope.lbl_break;
		auto pc              = scope.lbl_continue;
		auto pr              = scope.reg_break;
		auto ob              = scope.owner_break;
		auto oc              = scope.owner_continue;
		auto loop_break      = scope.make_label();
		auto natural_exit    = scope.make_label();
		scope.lbl_break      = loop_break;
		scope.lbl_continue   = scope.make_label();
		scope.owner_break    = &scope;
		scope.owner_continue = &scope;

		// Point the continue label at the beginning.
		//
		scope.set_label_here(scope.lbl_continue);

		// Parse the condition.
		//
		auto cc = expr_parse(scope);
		if (cc.kind == expr::err) {
			return {};
		}
		cc = cc.to_anyreg(scope);

		// Reserve next register for break-with-value, initialize to nil.
		//
		expression result = expression{nil}.to_nextreg(scope);
		scope.reg_break   = result.reg;
		scope.emit(bc::JNS, natural_exit, cc.reg);

		// Parse the block.
		//
		if (scope.lex().check('{') == lex::token_error) {
			return {};
		}
		auto body_base = scope.reg_next;
		if (expr_block(scope).kind == expr::err) {
			return {};
		}
		scope.discard_regs(body_base);
		scope.emit(bc::JMP, scope.lbl_continue);

		// Restore outer loop control before parsing else.
		//
		scope.set_label_here(natural_exit);
		scope.lbl_break      = pb;
		scope.lbl_continue   = pc;
		scope.reg_break      = pr;
		scope.owner_break    = ob;
		scope.owner_continue = oc;
		if (!parse_loop_else(scope, result.reg)) {
			return {};
		}

		scope.set_label_here(loop_break);
		return result;
	}
	static expression parse_for(func_scope& scope) {
		// Parse the iterator names.
		//
		string* k = nullptr;
		if (auto kn = scope.lex().check(lex::token_name); kn == lex::token_error) {
			return {};
		} else {
			k = kn.str_val;
		}
		string* v = nullptr;
		if (scope.lex().opt(',')) {
			if (auto vn = scope.lex().check(lex::token_name); vn == lex::token_error) {
				return {};
			} else {
				v = vn.str_val;
			}
		}

		// Expect in.
		//
		if (scope.lex().check(lex::token_in) == lex::token_error) {
			return {};
		}

		// Parse the iterable without consuming a range suffix. Constant, closed,
		// exact-integer ranges can use the numeric loop; every other range is an
		// ordinary range.create result and therefore keeps native validation and
		// iterator semantics.
		//
		expression          i = expr_parse_core(scope);
		parsed_range_suffix range_suffix;
		bool                specialized_range = false;
		if (i.kind == expr::err)
			return {};
		if (can_continue_expression(scope) && (scope.lex().tok == lex::token_range || scope.lex().tok == lex::token_rangei)) {
			if (!parse_range_suffix(scope, range_suffix, true))
				return {};

			constexpr number largest_exact_integer = 9007199254740992.0;
			auto             exact_integer         = [&](const expression& value) -> std::optional<number> {
				if (value.kind != expr::imm || !value.imm.is_num())
					return std::nullopt;
				const number result = value.imm.as_num();
				if (!std::isfinite(result) || std::trunc(result) != result || std::abs(result) > largest_exact_integer)
					return std::nullopt;
				return result;
			};

			const auto start_number = exact_integer(i);
			const auto end_number   = exact_integer(range_suffix.end);
			const auto step_number =
				 range_suffix.step.kind == expr::imm && range_suffix.step.imm == nil ? std::optional<number>{1} : exact_integer(range_suffix.step);
			const bool atomic_numeric_range = scope.fn.atomic_parse_depth && v == nullptr && step_number && *step_number != 0;
			specialized_range               = atomic_numeric_range || (v == nullptr && start_number && end_number && step_number && *step_number != 0 &&
																							  std::abs(*step_number) <= largest_exact_integer - std::abs(*end_number));

			if (!specialized_range) {
				i = emit_range_create(scope, i, range_suffix);
				if (i.kind == expr::err)
					return {};
			}
		}

		// Ordinary code specializes only the proven-safe constant shape. Atomic
		// plans also lower numeric-local bounds directly so no iterator call or
		// allocation occurs inside the transaction.
		//
		if (specialized_range) {
			expression   i2          = range_suffix.end;
			expression   step_value  = range_suffix.step.kind == expr::imm && range_suffix.step.imm == nil ? expression(any(number(1))) : range_suffix.step;
			const number step_number = step_value.imm.as_num();

			// Allocate 5 consequtive registers:
			// [it], [max], [step], [<cc>] [<result>]
			//
			auto iter_base = scope.alloc_reg(5);
			i.to_reg(scope, iter_base);
			i2.to_reg(scope, iter_base + 1);
			step_value.to_reg(scope, iter_base + 2);
			scope.set_reg(iter_base + 4, nil);

			// Allocate distinct natural-exit and break paths.
			//
			auto pb            = scope.lbl_break;
			auto pc            = scope.lbl_continue;
			auto pr            = scope.reg_break;
			auto ob            = std::exchange(scope.owner_break, &scope);
			auto oc            = std::exchange(scope.owner_continue, &scope);
			auto loop_break    = scope.make_label();
			auto natural_exit  = scope.make_label();
			scope.lbl_break    = loop_break;
			scope.lbl_continue = scope.make_label();
			scope.reg_break    = iter_base + 4;
			scope.add_local_at(k, true, iter_base);

			// Skip the AADD on first entry.
			//
			scope.emit(bc::JMP, 1);

			// Point the continue label at the beginning.
			//
			scope.set_label_here(scope.lbl_continue);

			// Parse the condition, jump to the else path if we reached the end.
			//
			scope.emit(bc::AADD, iter_base, iter_base, iter_base + 2);  // it = it + step
			const auto past_end = step_number > 0 ? (range_suffix.inclusive ? bc::CGT : bc::CGE) : (range_suffix.inclusive ? bc::CLT : bc::CLE);
			scope.emit(past_end, iter_base + 3, iter_base, iter_base + 1);
			scope.emit(bc::JS, natural_exit, iter_base + 3);

			// Parse the block.
			//
			if (scope.lex().check('{') == lex::token_error) {
				return {};
			}
			auto body_base = scope.reg_next;
			if (expr_block(scope).kind == expr::err) {
				return {};
			}
			scope.discard_regs(body_base);
			scope.emit(bc::JMP, scope.lbl_continue);

			// The iterator is out of scope in else, and break skips else.
			//
			scope.set_label_here(natural_exit);
			scope.remove_last_local();
			scope.lbl_break      = pb;
			scope.lbl_continue   = pc;
			scope.reg_break      = pr;
			scope.owner_break    = ob;
			scope.owner_continue = oc;
			if (!parse_loop_else(scope, iter_base + 4)) {
				return {};
			}

			scope.set_label_here(loop_break);
			scope.clear_regs(iter_base, 4);
			return expression(iter_base + 4);
		}
		// If enumerating for:
		//
		else {
			// Make sure its followed by a block.
			//
			if (scope.lex().check('{') == lex::token_error) {
				return {};
			}

			// Move table to next register, will be assigned the result on completion of the inner scope.
			//
			bc::reg tbl_reg = i.to_nextreg(scope);

			// Start a new scope.
			//
			{
				func_scope iscope{scope.fn};
				auto       pb           = iscope.lbl_break;
				auto       pc           = iscope.lbl_continue;
				auto       pr           = iscope.reg_break;
				auto       loop_break   = scope.make_label();
				auto       natural_exit = scope.make_label();
				iscope.lbl_break        = loop_break;
				iscope.lbl_continue     = scope.make_label();
				iscope.owner_break      = &iscope;
				iscope.owner_continue   = &iscope;

				// Allocate 4 consequtive registers:
				// [<istate>, k, v, <result>]
				//
				auto iter_base   = iscope.alloc_reg(4);
				iscope.reg_break = iter_base + 3;

				// Initialize istate and the result.
				//
				expression(any_t{0}).to_reg(iscope, iter_base);
				expression(nil).to_reg(iscope, iter_base + 3);

				// Assign the locals.
				//
				iscope.add_local_at(k, false, iter_base + (v ? 1 : 2));
				if (v) {
					iscope.add_local_at(v, false, iter_base + 2);
				}

				// Point the continue label at the beginning.
				//
				iscope.set_label_here(iscope.lbl_continue);

				// Iterate, jumping to the else path at exhaustion.
				//
				iscope.emit(bc::ITER, natural_exit, iter_base, tbl_reg);

				auto body_base = iscope.reg_next;
				if (expr_block(iscope).kind == expr::err) {
					return {};
				}
				iscope.discard_regs(body_base);
				iscope.emit(bc::JMP, iscope.lbl_continue);

				// Iterator bindings do not escape into else; break skips it.
				//
				iscope.set_label_here(natural_exit);
				iscope.clear_locals();
				iscope.lbl_break    = pb;
				iscope.lbl_continue = pc;
				iscope.reg_break    = pr;
				if (!parse_loop_else(iscope, iter_base + 3)) {
					return {};
				}

				iscope.set_label_here(loop_break);
				expression(iter_base + 3).to_reg(iscope, tbl_reg);
				iscope.discard_regs(iter_base);
			}

			// Return the result.
			//
			return tbl_reg;
		}
	}
	static expression parse_try(func_scope& scope) {
		// Allocate the labels and a register for the catchpad.
		//
		auto catch_pad         = scope.make_label();
		auto no_throw          = scope.make_label();
		auto pc                = scope.lbl_catchpad;
		auto pb                = scope.catchpad_cleanup;
		scope.lbl_catchpad     = catch_pad;
		scope.catchpad_cleanup = false;

		emit_handler_state(scope, catch_pad, false, scope.lbl_cleanup);

		// Reserve next register for block result, initialize to nil.
		//
		auto result = expression{nil}.to_nextreg(scope);

		// Parse the try block.
		//
		if (scope.lex().check('{') == lex::token_error) {
			return {};
		}
		if (expr_block(scope, result).kind == expr::err) {
			return {};
		}

		// Normal completion leaves this try's protected region before joining
		// the continuation. Restore the outer handler on both CFG edges.
		//
		emit_handler_state(scope, pc, pb, scope.lbl_cleanup);
		scope.emit(bc::JMP, no_throw);

		// Set the catchpad.
		//
		scope.set_label_here(scope.lbl_catchpad);

		// Restore the label.
		//
		scope.lbl_catchpad     = pc;
		scope.catchpad_cleanup = pb;
		emit_handler_state(scope, pc, pb, scope.lbl_cleanup);

		// Parse the catch block.
		//
		if (scope.lex().opt(lex::token_catch)) {
			func_scope iscope{scope.fn};

			if (auto errv = iscope.lex().opt(lex::token_name)) {
				auto r = iscope.add_local(errv->str_val, false);
				iscope.emit(bc::GETEX, r);
			}
			if (iscope.lex().check('{') == lex::token_error) {
				return {};
			}
			if (expr_block(iscope, result).kind == expr::err) {
				return {};
			}
		} else {
			scope.emit(bc::GETEX, result);
		}

		// Emit the result label.
		//
		scope.set_label_here(no_throw);

		// Return the result.
		//
		return result;
	}

	// Parses the code and returns it as a function instance with no arguments on success.
	//
	any load_script(vm* L, std::string_view source, std::string_view source_name, std::string_view module_name, bool is_repl) {
		// Handle UTF input.
		//
		std::string temp;
		if (util::utf_is_bom(source)) [[unlikely]] {
			temp   = util::utf_convert<char>(std::span((const uint8_t*) source.data(), source.size()));
			source = temp;
		}

		// Initialize compilation-lifetime roots, the lexer, and the function
		// state. No collector suspension or deferred ownership is involved.
		//
		parser_roots roots{L};
		lex::state   lx{L, source, source_name};
		func_state   fn{L, lx, roots, is_repl};
		bool         published_module = false;

		// A runtime loader publishes the export table and a loading record before
		// compiling. Direct load_script callers still get the legacy behavior of
		// creating a new module table, but may not replace an initialized module.
		//
		if (!module_name.empty() && !is_repl) {
			auto* mod = string::create(L, module_name);
			fn.set_module_name(mod);
			rc::release(L, mod);

			any exists = L->modules->get(L, any(fn.module_name));
			if (exists != nil) {
				any record = L->module_records ? any(L->module_records->get(L, any(fn.module_name))) : nil;
				any status = record.is_tbl() ? any(record.as_tbl()->get(L, any(number(0)))) : nil;
				if (!exists.is_tbl() || !status.is_num() || status.as_num() != 0) {
					L->error("module '%s' already exists.", fn.module_name->c_str());
					return exception_marker;
				}
				fn.set_module_table(exists.as_tbl());
			} else {
				fn.adopt_module_table(table::create(L));
				if (!L->modules->set(L, any(fn.module_name), any(fn.module_table)))
					return exception_marker;
				published_module = true;
			}
		}

		// REPL and compiler environment tables are explicit owning roots.
		//
		if (is_repl) {
			if (!L->repl_scope)
				L->repl_scope = table::create(L);
			fn.set_scope_table(L->repl_scope);
		} else {
			fn.adopt_scope_table(table::create(L));
		}
		fn.adopt_type_table(table::create(L));

		// Parse the body and transfer a successful write_func result to the
		// caller. func_state and parser_roots unwind every other owned value.
		//
		if (!parse_body(fn)) {
			if (published_module)
				L->modules->erase(L, any(fn.module_name));
			L->error("%s", fn.lex.last_error.c_str());
			return exception_marker;
		}
		fn.type_table->is_frozen = true;
		if (auto* result = write_func(fn, 0)) {
			if (!is_repl) {
				table* bindings     = fn.module_table ? fn.module_table : fn.scope_table;
				bindings->is_frozen = true;
			}
			return result;
		}

		if (published_module)
			L->modules->erase(L, any(fn.module_name));
		L->error("%s", fn.lex.last_error.c_str());
		return exception_marker;
	}
};
