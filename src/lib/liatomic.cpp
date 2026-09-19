#include <algorithm>
#include <cmath>
#include <functional>
#include <util/user.hpp>
#include <vector>
#include <vm/array.hpp>
#include <vm/atomic.hpp>
#include <vm/function.hpp>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/shared.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <vm/types.hpp>

namespace li::atomic {
	namespace {
		const char* invalid_register(const function_proto* prototype, const bc::insn& instruction) noexcept {
			const auto& descriptor = bc::opcode_details(instruction.o);
			auto        invalid    = [&](bc::op_t kind, bc::reg value) { return kind == bc::op_t::reg && (value < 0 || msize_t(value) >= prototype->num_locals); };
			return invalid(descriptor.a, instruction.a) || invalid(descriptor.b, instruction.b) || invalid(descriptor.c, instruction.c)
						  ? "atomic block accesses a value outside its restricted frame"
						  : nullptr;
		}

		bool scalar_value(any_t value) noexcept { return value == nil || value.is_num() || value.is_bool(); }

		struct staged_field {
			any    target = nil;
			any    key    = nil;
			number value  = 0;
			bool   dirty  = false;
		};

		const char* read_numeric_field(any_t target, any_t key, number& result) noexcept {
			any value = nil;
			if (!target.is_gc() || !shared::is_shared(target))
				return "atomic field access requires a shared target";
			if (target.is_tbl()) {
				if (key == nil)
					return "atomic table key cannot be nil";
				value = target.as_tbl()->get(nullptr, key);
			} else if (target.is_arr()) {
				if (!key.is_num() || !std::isfinite(key.as_num()) || key.as_num() < 0 || key.as_num() != std::trunc(key.as_num()) ||
					 key.as_num() >= target.as_arr()->size())
					return "atomic array index is out of bounds";
				value = target.as_arr()->get(nullptr, msize_t(key.as_num()));
			} else if (target.is_obj()) {
				if (!key.is_str())
					return "atomic object field key must be a string";
				bool found = false;
				for (const field_pair& field : target.as_obj()->cl->fields()) {
					if (!string_value_equals(field.key, key.as_str()))
						continue;
					if (field.value.is_static)
						return "atomic updates of static class fields require the class target";
					found = true;
					break;
				}
				if (!found)
					return "atomic object field does not exist";
				value = target.as_obj()->get(key.as_str());
			} else {
				return "atomic field access requires a shared table, array, or object";
			}
			if (!value.is_num())
				return "atomic field is not numeric";
			result = value.as_num();
			return nullptr;
		}

		staged_field* find_field(std::vector<staged_field>& fields, any_t target, any_t key) noexcept {
			for (staged_field& field : fields) {
				if (field.target.value == target.value && any_value_equals(field.key, key))
					return &field;
			}
			return nullptr;
		}

		const char* load_field(std::vector<staged_field>& fields, size_t capacity, any_t target, any_t key, number& result) noexcept {
			if (staged_field* field = find_field(fields, target, key)) {
				result = field->value;
				return nullptr;
			}
			if (fields.size() == capacity)
				return "atomic block accesses too many distinct fields for its validated plan";
			if (const char* error = read_numeric_field(target, key, result))
				return error;
			fields.push_back({any(target), any(key), result, false});
			return nullptr;
		}

		const char* store_field(std::vector<staged_field>& fields, size_t capacity, any_t target, any_t key, any_t value) noexcept {
			if (!value.is_num())
				return "atomic field writes must be numeric";
			number ignored;
			if (const char* error = load_field(fields, capacity, target, key, ignored))
				return error;
			staged_field* field = find_field(fields, target, key);
			LI_ASSERT(field != nullptr);
			field->value = value.as_num();
			field->dirty = true;
			return nullptr;
		}

		any_t LI_CC make_shared_value(vm* L, any* args, slot_t count) {
			if (count != 1)
				return L->error("shared factory expects one value");
			return shared::make(L, args[0]);
		}

		any_t LI_CC lock_shared_value(vm* L, any* args, slot_t count) {
			if (count != 1 || !args[0].is_gc() || !shared::is_shared(args[0]))
				return L->error("lock expects a shared value");
			gc::header* target = args[0].as_gc();
			shared::retain(target);
			shared::lock(L, target);
			return L->ok();
		}

		any_t LI_CC unlock_shared_value(vm* L, any* args, slot_t count) {
			if (count != 1 || !args[0].is_gc() || !shared::is_shared(args[0]))
				return L->error("lock cleanup expects a shared value");
			gc::header* target = args[0].as_gc();
			if (!shared::held_by_current_thread(target))
				return L->error("shared lock cleanup lost lock ownership");
			shared::unlock(L, target);
			shared::release(L, target);
			return L->ok();
		}

		any_t LI_CC capture_value(vm* L, any* args, slot_t count) {
			if (count != 2 || !args[0].is_fn() || !args[0].as_fn()->is_virtual() || !args[-1].is_num())
				return L->error("invalid atomic capture lookup");
			number    index   = args[-1].as_num();
			function* closure = args[0].as_fn();
			if (!std::isfinite(index) || index < 0 || index != std::trunc(index) || index >= closure->num_uval)
				return L->error("atomic capture index is out of bounds");
			return L->ok(closure->uvals()[msize_t(index)]);
		}

		any_t LI_CC execute_plan(vm* L, any* args, slot_t count) {
			if (count != 1 || !args[0].is_fn() || !args[0].as_fn()->is_virtual())
				return L->error("invalid atomic transaction plan");
			function*       closure   = args[0].as_fn();
			function_proto* prototype = closure->proto;
			if (const char* error = validate_plan(prototype))
				return L->error("%s", error);

			const auto opcodes        = prototype->opcodes();
			size_t     field_capacity = 0;
			for (const bc::insn& instruction : opcodes) {
				if (instruction.o == bc::TGET || instruction.o == bc::TSET)
					++field_capacity;
			}

			// Every allocation and every captured-value check precedes the first
			// target lock. The vectors are then used only within their fixed capacity.
			std::vector<any>         registers(prototype->num_locals, nil);
			std::vector<any>         captures(closure->uvals().begin(), closure->uvals().end());
			std::vector<bool>        capture_changed(closure->num_uval, false);
			std::vector<gc::header*> targets;
			targets.reserve(closure->num_uval);
			std::vector<staged_field> fields;
			fields.reserve(field_capacity);
			std::vector<shared::numeric_update> updates(field_capacity);
			std::vector<number>                 update_results(field_capacity);

			for (any value : captures) {
				if (value.is_num())
					continue;
				if (!value.is_gc() || !shared::is_shared(value) || (!value.is_tbl() && !value.is_arr() && !value.is_obj()))
					return L->error("atomic captures must be numeric locals or shared field targets");
				targets.push_back(value.as_gc());
			}
			std::sort(targets.begin(), targets.end(), std::less<gc::header*>{});
			targets.erase(std::unique(targets.begin(), targets.end()), targets.end());

			for (gc::header* target : targets)
				shared::lock(L, target);
			auto unlock_targets = [&] {
				for (auto it = targets.rbegin(); it != targets.rend(); ++it)
					shared::unlock(L, *it);
			};

			const char* error  = nullptr;
			any         result = nil;
			size_t      pc     = 0;
			while (!error && pc < opcodes.size()) {
				const bc::insn& instruction = opcodes[pc++];
				const bc::reg   a           = instruction.a;
				const bc::reg   b           = instruction.b;
				const bc::reg   c           = instruction.c;
				switch (instruction.o) {
					case bc::NOP:
						break;
					case bc::KIMM:
						registers[a] = any(std::in_place, instruction.xmm());
						break;
					case bc::MOV:
						registers[a] = registers[b];
						break;
					case bc::UGET:
						registers[a] = captures[b];
						break;
					case bc::USET:
						if (!closure->uvals()[a].is_num() || !registers[b].is_num()) {
							error = "atomic local writes must remain numeric";
							break;
						}
						captures[a]        = registers[b];
						capture_changed[a] = true;
						break;
					case bc::ANEG:
						if (!registers[b].is_num())
							error = "atomic arithmetic requires numeric operands";
						else
							registers[a] = any(-registers[b].as_num());
						break;
					case bc::LNOT:
					case bc::TOBOOL:
						if (!scalar_value(registers[b]))
							error = "atomic conditions require numeric or boolean values";
						else
							registers[a] = any(instruction.o == bc::LNOT ? !registers[b].coerce_bool() : registers[b].coerce_bool());
						break;
					case bc::AADD:
					case bc::ASUB:
					case bc::AMUL:
					case bc::ADIV:
					case bc::AMOD:
					case bc::APOW: {
						if (!registers[b].is_num() || !registers[c].is_num()) {
							error = "atomic arithmetic requires numeric operands";
							break;
						}
						const number lhs = registers[b].as_num();
						const number rhs = registers[c].as_num();
						number       value;
						switch (instruction.o) {
							case bc::AADD:
								value = lhs + rhs;
								break;
							case bc::ASUB:
								value = lhs - rhs;
								break;
							case bc::AMUL:
								value = lhs * rhs;
								break;
							case bc::ADIV:
								value = lhs / rhs;
								break;
							case bc::AMOD:
								value = std::fmod(lhs, rhs);
								break;
							case bc::APOW:
								value = std::pow(lhs, rhs);
								break;
							default:
								assume_unreachable();
						}
						registers[a] = any(value);
						break;
					}
					case bc::LAND:
					case bc::LOR:
					case bc::NCS:
						if (!scalar_value(registers[b]) || !scalar_value(registers[c])) {
							error = "atomic logical operations require scalar values";
							break;
						}
						if (instruction.o == bc::LAND)
							registers[a] = registers[b].coerce_bool() ? registers[c] : registers[b];
						else if (instruction.o == bc::LOR)
							registers[a] = registers[b].coerce_bool() ? registers[b] : registers[c];
						else
							registers[a] = registers[b] == nil ? registers[c] : registers[b];
						break;
					case bc::CEQ:
					case bc::CNE:
						if (!scalar_value(registers[b]) || !scalar_value(registers[c]))
							error = "atomic comparisons require scalar values";
						else
							registers[a] = any(instruction.o == bc::CEQ ? registers[b] == registers[c] : registers[b] != registers[c]);
						break;
					case bc::CLT:
					case bc::CGE:
					case bc::CGT:
					case bc::CLE: {
						if (!registers[b].is_num() || !registers[c].is_num()) {
							error = "atomic comparisons require numeric operands";
							break;
						}
						const number lhs = registers[b].as_num();
						const number rhs = registers[c].as_num();
						bool         value;
						switch (instruction.o) {
							case bc::CLT:
								value = lhs < rhs;
								break;
							case bc::CGE:
								value = lhs >= rhs;
								break;
							case bc::CGT:
								value = lhs > rhs;
								break;
							case bc::CLE:
								value = lhs <= rhs;
								break;
							default:
								assume_unreachable();
						}
						registers[a] = any(value);
						break;
					}
					case bc::CTY:
						registers[a] = any(registers[b].type() == value_type(c));
						break;
					case bc::TGET: {
						number value;
						error = load_field(fields, field_capacity, registers[c], registers[b], value);
						if (!error)
							registers[a] = any(value);
						break;
					}
					case bc::TSET:
						error = store_field(fields, field_capacity, registers[c], registers[a], registers[b]);
						break;
					case bc::JMP:
						pc = size_t(int64_t(pc) + a);
						break;
					case bc::JS:
					case bc::JNS:
						if (!scalar_value(registers[b])) {
							error = "atomic branches require numeric or boolean conditions";
							break;
						}
						if (registers[b].coerce_bool() == (instruction.o == bc::JS))
							pc = size_t(int64_t(pc) + a);
						break;
					case bc::RET:
						result = registers[a];
						pc     = opcodes.size();
						break;
					default:
						error = "atomic plan contains an unsupported instruction";
						break;
				}
			}

			if (!error && !scalar_value(result))
				error = "atomic block result must be numeric, boolean, or nil";

			size_t update_count = 0;
			if (!error) {
				for (const staged_field& field : fields) {
					if (!field.dirty)
						continue;
					updates[update_count++] = {
						 .target    = field.target.as_gc(),
						 .key       = field.key,
						 .operation = shared::numeric_operation::set,
						 .operand   = field.value,
					};
				}
			}

			if (!error && update_count != 0 && !shared::atomic_update(L, {updates.data(), update_count}, {update_results.data(), update_count})) {
				unlock_targets();
				return L->error("atomic transaction validation failed");
			}
			if (!error) {
				for (size_t index = 0; index != captures.size(); ++index) {
					if (capture_changed[index])
						rc::replace(L, closure->uvals()[index], captures[index]);
				}
			}
			unlock_targets();
			if (error)
				return L->error("%s", error);
			return L->ok(result);
		}
	}

	const char* validate_plan(function_proto* prototype) noexcept {
		if (!prototype)
			return "atomic block has no transaction prototype";
		const auto opcodes = prototype->opcodes();
		for (size_t pc = 0; pc != opcodes.size(); ++pc) {
			const bc::insn& instruction = opcodes[pc];
			if (uint8_t(instruction.o) >= std::size(bc::opcode_descs))
				return "atomic block contains invalid bytecode";
			if (const char* error = invalid_register(prototype, instruction))
				return error;
			auto valid_target = [&](bc::rel offset) {
				const int64_t destination = int64_t(pc) + 1 + offset;
				return destination >= 0 && destination < int64_t(opcodes.size());
			};
			switch (instruction.o) {
				case bc::NOP:
				case bc::LNOT:
				case bc::ANEG:
				case bc::MOV:
				case bc::AADD:
				case bc::ASUB:
				case bc::AMUL:
				case bc::ADIV:
				case bc::AMOD:
				case bc::APOW:
				case bc::LAND:
				case bc::LOR:
				case bc::NCS:
				case bc::CTY:
				case bc::CEQ:
				case bc::CNE:
				case bc::CLT:
				case bc::CGE:
				case bc::CGT:
				case bc::CLE:
				case bc::TGET:
				case bc::TSET:
				case bc::TOBOOL:
				case bc::RET:
					break;
				case bc::KIMM: {
					any value(std::in_place, instruction.xmm());
					if (value.is_gc() && !value.is_str())
						return "atomic blocks may only embed numeric constants and field names";
					break;
				}
				case bc::UGET:
					if (instruction.b < 0 || msize_t(instruction.b) >= prototype->num_uval)
						return "atomic block contains an invalid capture read";
					break;
				case bc::USET:
					if (instruction.a < 0 || msize_t(instruction.a) >= prototype->num_uval)
						return "atomic block contains an invalid capture write";
					break;
				case bc::JMP:
					if (!valid_target(instruction.a))
						return "atomic block contains an invalid branch";
					break;
				case bc::JS:
				case bc::JNS:
					if (!valid_target(instruction.a))
						return "atomic block contains an invalid conditional branch";
					break;
				case bc::CALL:
					return "calls are not allowed inside atomic blocks";
				case bc::ANEW:
				case bc::TNEW:
				case bc::FDUP:
				case bc::STRIV:
				case bc::CCAT:
					return "allocation is not allowed inside atomic blocks";
				case bc::SETEX:
				case bc::GETEX:
				case bc::SETEH:
					return "throw and exception handling are not allowed inside atomic blocks";
				case bc::ITER:
					return "iterator calls are not allowed inside atomic blocks";
				default:
					return "instruction is not allowed inside atomic blocks";
			}
		}
		return nullptr;
	}

	util::native_function detail::make_shared = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 "shared.create",
		 &make_shared_value,
	};
	util::native_function detail::lock_shared = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 "shared.lock",
		 &lock_shared_value,
	};
	util::native_function detail::unlock_shared = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 "shared.unlock",
		 &unlock_shared_value,
	};
	util::native_function detail::execute = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 nullptr,
		 &execute_plan,
	};
	util::native_function detail::capture = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 nullptr,
		 &capture_value,
	};
}
