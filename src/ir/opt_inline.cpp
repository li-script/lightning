#include <ir/bc2ir.hpp>
#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>
#include <iterator>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace li::ir::opt {
	namespace {
		struct prepared_inline {
			std::unique_ptr<procedure>               body;
			std::unordered_map<const value*, value*> external_values;
			std::vector<ref<insn>>                   placeholders;
			std::unordered_set<const insn*>          placeholder_set;
			size_t                                   cloned_instructions = 0;
			size_t                                   return_count        = 0;
		};

		bool is_inline_value_type(type value_type) {
			return value_type == type::nil || value_type == type::i1 || is_integer_data(value_type) || is_floating_point_data(value_type);
		}

		function* resolve_script_target(value* target) {
			std::unordered_set<value*> visited;
			while (target->is<insn>() && visited.emplace(target).second) {
				auto* instruction = target->as<insn>();
				if (instruction->is<move>() || instruction->is<assume_cast>() || instruction->is<erase_type>()) {
					target = instruction->operands[0].get();
					continue;
				}
				if (instruction->is<phi>() && !instruction->operands.empty()) {
					auto* first = instruction->operands.front().get();
					bool  same  = true;
					for (const auto& operand : instruction->operands)
						same &= operand.get() == first;
					if (same) {
						target = first;
						continue;
					}
				}
				break;
			}
			if (!target->is<constant>() || target->vt != type::fn)
				return nullptr;
			auto* function = target->as<constant>()->fn;
			return function && function->is_virtual() ? function : nullptr;
		}

		bool traces_frame_slot(value* source, bc::reg slot, std::unordered_set<value*>& visited) {
			if (!source->is<insn>() || !visited.emplace(source).second)
				return false;
			auto* instruction = source->as<insn>();
			if (instruction->is<load_local>())
				return instruction->operands[0]->as<constant>()->i32 == slot;
			if (instruction->is<move>() || instruction->is<assume_cast>() || instruction->is<erase_type>())
				return traces_frame_slot(instruction->operands[0].get(), slot, visited);
			return false;
		}

		bool remove_redundant_frame_spills(procedure* body) {
			std::vector<insn*> redundant;
			for (auto& block : body->basic_blocks) {
				for (auto* instruction : block->insns()) {
					if (!instruction->is<store_local>())
						continue;
					auto slot = instruction->operands[0]->as<constant>()->i32;
					if (slot >= 0)
						continue;
					std::unordered_set<value*> visited;
					if (!traces_frame_slot(instruction->operands[1].get(), slot, visited))
						return false;
					redundant.emplace_back(instruction);
				}
			}
			for (auto* instruction : redundant)
				instruction->erase();
			return true;
		}

		ref<constant> placeholder_constant(procedure* body, type value_type) {
			switch (value_type) {
				case type::nil:
					return body->add_const(constant(nil));
				case type::i1:
					return body->add_const(constant(false));
				case type::i8:
					return body->add_const(constant(int8_t(0)));
				case type::i16:
					return body->add_const(constant(int16_t(0)));
				case type::i32:
					return body->add_const(constant(int32_t(0)));
				case type::i64:
					return body->add_const(constant(int64_t(0)));
				case type::f32:
					return body->add_const(constant(float(0)));
				case type::f64:
					return body->add_const(constant(double(0)));
				default:
					return nullptr;
			}
		}

		ref<> make_input(prepared_inline& prepared, value* actual) {
			if (actual->is<constant>()) {
				auto copy = *actual->as<constant>();
				if (copy.vt != type::bb)
					return prepared.body->add_const(copy);
			}
			if (!is_inline_value_type(actual->vt))
				return nullptr;
			auto initial = placeholder_constant(prepared.body.get(), actual->vt);
			if (!initial)
				return nullptr;
			auto placeholder = builder{prepared.body->get_entry()}.emit_front<move>(initial);
			prepared.external_values.emplace(placeholder.get(), actual);
			prepared.placeholder_set.emplace(placeholder.get());
			prepared.placeholders.emplace_back(placeholder);
			return placeholder;
		}

		void refresh_types(procedure* body) {
			size_t instruction_count = 0;
			for (const auto& block : body->basic_blocks)
				instruction_count += size_t(std::distance(block->begin(), block->end()));
			for (size_t iteration = 0; iteration != instruction_count + 1; ++iteration) {
				bool changed = false;
				for (auto& block : body->basic_blocks) {
					for (auto* instruction : block->insns()) {
						auto before = instruction->vt;
						instruction->update();
						changed |= before != instruction->vt;
					}
				}
				if (!changed)
					break;
			}
		}

		bool simplify_proven_values(procedure* body) {
			bool changed = false;
			for (auto& block : body->basic_blocks) {
				for (auto it = block->begin(); it != block->end();) {
					auto* instruction = *it++;
					ref<> replacement;
					if (instruction->is<extract>()) {
						auto* source = instruction->operands[0].get();
						auto  index  = instruction->operands[1]->as<constant>()->i32;
						if (source->vt != type::any) {
							if (index == 0)
								replacement = make_ref(source);
							else
								replacement = body->add_const(source->vt != type::exc);
						}
					} else if (instruction->is<test_type>() && instruction->operands[0]->vt != type::any) {
						auto actual   = to_value_type(instruction->operands[0]->vt);
						auto expected = instruction->operands[1]->as<constant>()->vty;
						replacement   = body->add_const(actual == expected);
					} else if (instruction->is<assume_cast>() && instruction->operands[0]->vt == instruction->vt) {
						replacement = instruction->operands[0];
					}
					if (!replacement)
						continue;
					instruction->replace_all_uses(replacement.get());
					instruction->erase();
					changed = true;
				}
			}
			return changed;
		}

		bool is_safe_instruction(const insn* instruction) {
			if (instruction->is<ret>())
				return is_inline_value_type(instruction->operands[0]->vt);
			if (instruction->is<jmp>())
				return true;
			if (instruction->is<jcc>())
				return instruction->operands[0]->vt == type::i1;
			if (instruction->is<phi>())
				return is_inline_value_type(instruction->vt);
			if (instruction->is<move>() || instruction->is<assume_cast>() || instruction->is<erase_type>())
				return is_inline_value_type(instruction->vt) && is_inline_value_type(instruction->operands[0]->vt);
			if (instruction->is<keep_alive>())
				return is_inline_value_type(instruction->operands[0]->vt);
			// Rejecting every remaining call is intentional: it keeps recursion and
			// mutual recursion bounded without cloning a partial call chain, and it
			// keeps error paths on real callee frames.
			if (!detail::is_mathematical_scalar(instruction))
				return false;
			return !detail::may_trap_target_integer(instruction);
		}

		std::optional<prepared_inline> prepare_inline(procedure* caller, vcall* call, function* target) {
			auto* prototype = target->proto;
			if (!prototype || target->num_uval != 0 || prototype->num_uval != 0 || prototype->length == 0)
				return std::nullopt;

			const auto& prologue = prototype->opcode_array[0];
			if (prologue.o != bc::VACHK)
				return std::nullopt;
			const size_t actual_count = call->operands.size() - 2;
			if (actual_count != size_t(prologue.a))
				return std::nullopt;

			// Optional/default/rest contracts observe the real argument count. Exact-arity
			// fixed calls need only the VACHK count; other vararg operations stay as calls.
			for (const auto& bytecode : prototype->opcodes()) {
				if (bytecode.o == bc::VACNT || bytecode.o == bc::VAGET)
					return std::nullopt;
			}
			for (size_t index = 2; index != call->operands.size(); ++index) {
				if (!is_inline_value_type(call->operands[index]->vt))
					return std::nullopt;
			}

			prepared_inline prepared;
			prepared.body = lift_bc(caller->L, prototype);
			if (!remove_redundant_frame_spills(prepared.body.get()))
				return std::nullopt;

			std::unordered_map<bc::reg, ref<>> inputs;
			inputs.emplace(FRAME_TARGET, prepared.body->add_const(constant(target)));
			if (auto self = make_input(prepared, call->operands[1].get()))
				inputs.emplace(FRAME_SELF, std::move(self));
			else
				return std::nullopt;
			for (size_t index = 0; index != actual_count; ++index) {
				auto input = make_input(prepared, call->operands[index + 2].get());
				if (!input)
					return std::nullopt;
				inputs.emplace(bc::reg(-FRAME_SIZE - 1 - bc::reg(index)), std::move(input));
			}

			for (auto& block : prepared.body->basic_blocks) {
				for (auto it = block->begin(); it != block->end();) {
					auto* instruction = *it++;
					if (instruction->is<load_local>()) {
						auto slot = instruction->operands[0]->as<constant>()->i32;
						if (slot >= 0)
							continue;
						auto input = inputs.find(slot);
						if (input == inputs.end())
							return std::nullopt;
						instruction->replace_all_uses(input->second.get());
						instruction->erase();
					} else if (instruction->is<va_count>()) {
						instruction->replace_all_uses(prepared.body->add_const(int32_t(actual_count)));
						instruction->erase();
					}
				}
			}

			refresh_types(prepared.body.get());
			lift_phi(prepared.body.get());
			for (size_t iteration = 0; iteration != 4; ++iteration) {
				refresh_types(prepared.body.get());
				bool simplified = simplify_proven_values(prepared.body.get());
				fold_constant(prepared.body.get());
				dce(prepared.body.get());
				cfg(prepared.body.get());
				if (!simplified)
					break;
			}
			refresh_types(prepared.body.get());

			for (const auto& block : prepared.body->basic_blocks) {
				for (auto* instruction : block->insns()) {
					if (prepared.placeholder_set.contains(instruction))
						continue;
					if (!is_safe_instruction(instruction))
						return std::nullopt;
					if (instruction->is<ret>())
						++prepared.return_count;
					else
						++prepared.cloned_instructions;
				}
			}
			if (prepared.return_count == 0)
				return std::nullopt;
			return prepared;
		}

		value* mapped_value(procedure* caller, value* source, const prepared_inline& prepared, const std::unordered_map<const basic_block*, basic_block*>& blocks,
			 const std::unordered_map<const insn*, insn*>& instructions) {
			if (auto external = prepared.external_values.find(source); external != prepared.external_values.end())
				return external->second;
			if (source->is<constant>()) {
				auto copy = *source->as<constant>();
				if (copy.vt == type::bb)
					copy.bb = blocks.at(copy.bb);
				return caller->add_const(copy).get();
			}
			return instructions.at(source->as<insn>());
		}

		bool inline_prepared(procedure* caller, vcall* call, const prepared_inline& prepared) {
			auto*         call_block   = call->parent;
			const bc::pos block_end    = call_block->bc_end;
			auto*         continuation = call_block->split_at(call);
			call_block->bc_end         = call->source_bc;
			continuation->bc_begin     = call->source_bc;
			continuation->bc_end       = block_end;
			std::unordered_map<const basic_block*, basic_block*> blocks;
			std::unordered_map<const insn*, insn*>               instructions;
			std::vector<std::pair<const ret*, basic_block*>>     returns;

			for (const auto& source : prepared.body->basic_blocks) {
				auto* destination       = caller->add_block();
				destination->cold_hint  = source->cold_hint;
				destination->loop_depth = source->loop_depth;
				destination->bc_begin   = call->source_bc;
				destination->bc_end     = call->source_bc;
				blocks.emplace(source.get(), destination);
			}

			for (const auto& source : prepared.body->basic_blocks) {
				auto* destination = blocks.at(source.get());
				for (auto* instruction : source->insns()) {
					if (prepared.placeholder_set.contains(instruction))
						continue;
					if (instruction->is<ret>()) {
						returns.emplace_back(instruction->as<ret>(), destination);
						continue;
					}
					ref<insn> copy(std::in_place, instruction->duplicate());
					copy->operands.clear();
					copy->name      = caller->next_reg_name++;
					copy->source_bc = call->source_bc;
					auto* raw       = copy.get();
					destination->push_back(std::move(copy));
					instructions.emplace(instruction, raw);
				}
			}

			for (const auto& source : prepared.body->basic_blocks) {
				for (auto* instruction : source->insns()) {
					if (instruction->is<ret>() || prepared.placeholder_set.contains(instruction))
						continue;
					auto* copy = instructions.at(instruction);
					for (const auto& operand : instruction->operands)
						copy->operands.emplace_back(make_ref(mapped_value(caller, operand.get(), prepared, blocks, instructions)));
					copy->update();
				}
			}

			// Preserve predecessor occurrence order exactly: PHI operand order is edge
			// identity, and rebuilding it by block iteration can silently swap values.
			for (const auto& source : prepared.body->basic_blocks) {
				auto* destination = blocks.at(source.get());
				for (auto* successor : source->successors)
					destination->successors.emplace_back(blocks.at(successor));
				for (auto* predecessor : source->predecessors)
					destination->predecessors.emplace_back(blocks.at(predecessor));
			}
			caller->mark_blocks_dirty();

			std::vector<value*> return_values;
			return_values.reserve(returns.size());
			for (const auto& [source_return, destination] : returns) {
				return_values.emplace_back(mapped_value(caller, source_return->operands[0].get(), prepared, blocks, instructions));
				builder at{destination};
				at.current_bc = call->source_bc;
				at.emit<jmp>(continuation);
				caller->add_jump(destination, continuation);
			}

			ref<> result;
			if (return_values.size() == 1) {
				result = make_ref(return_values.front());
			} else {
				auto joined = builder{continuation}.emit_front<phi>();
				for (auto* value : return_values)
					joined->operands.emplace_back(make_ref(value));
				joined->update();
				result = joined;
			}

			std::vector<ref<insn>> users;
			call->for_each_user([&](insn* user, size_t) {
				users.emplace_back(make_ref(user));
				return false;
			});
			for (auto& user : users) {
				if (!user->parent)
					continue;
				if (user->is<extract>() && user->operands[0].get() == call) {
					auto index = user->operands[1]->as<constant>()->i32;
					user->replace_all_uses(index == 0 ? result.get() : caller->add_const(true).get());
					user->erase();
					continue;
				}
				for (auto& operand : user->operands) {
					if (operand.get() == call)
						operand.reset(result.get());
				}
				user->update();
			}

			call->erase();
			builder enter{call_block};
			enter.current_bc = call->source_bc;
			enter.emit<jmp>(blocks.at(prepared.body->get_entry()));
			caller->add_jump(call_block, blocks.at(prepared.body->get_entry()));
			return true;
		}
	}

	size_t inline_calls(procedure* proc, size_t instruction_budget) {
		// This pass handles script bodies only. It must precede type_split_cfg so
		// specialize_native remains the single lowering path for known intrinsics.
		size_t                          transformed = 0;
		size_t                          remaining   = instruction_budget;
		std::unordered_set<const insn*> rejected;
		while (remaining != 0) {
			ref<vcall> candidate;
			function*  target = nullptr;
			for (auto& block : proc->basic_blocks) {
				for (auto* instruction : block->insns()) {
					if (!instruction->is<vcall>() || rejected.contains(instruction))
						continue;
					target = resolve_script_target(instruction->operands[0].get());
					if (!target)
						continue;
					candidate = make_ref(instruction->as<vcall>());
					break;
				}
				if (candidate)
					break;
			}
			if (!candidate)
				break;

			auto prepared = prepare_inline(proc, candidate.get(), target);
			if (!prepared) {
				rejected.emplace(candidate.get());
				continue;
			}

			const size_t additions = prepared->cloned_instructions + prepared->return_count + 1 + (prepared->return_count > 1 ? 1 : 0);
			const size_t growth    = additions > 0 ? additions - 1 : 0;
			if (growth > remaining) {
				rejected.emplace(candidate.get());
				continue;
			}
			inline_prepared(proc, candidate.get(), *prepared);
			remaining -= growth;
			++transformed;
		}
		return transformed;
	}
}
