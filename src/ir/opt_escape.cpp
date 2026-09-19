#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>
#include <vm/string.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace li::ir::opt {
	namespace {
		constexpr size_t max_aliases          = 64;
		constexpr size_t max_candidate_uses   = 256;
		constexpr size_t max_field_operations = 128;
		constexpr size_t max_tracked_fields   = 32;
		constexpr size_t max_dataflow_blocks  = 128;
		constexpr size_t max_scalar_phis      = 64;
		constexpr size_t no_field             = std::numeric_limits<size_t>::max();

		bool precedes_in_block(const insn* definition, const insn* use) {
			LI_ASSERT(definition->parent == use->parent);
			for (auto* instruction : definition->parent->insns()) {
				if (instruction == definition)
					return true;
				if (instruction == use)
					return false;
			}
			return false;
		}

		bool is_primitive_scalar(const value* candidate) {
			// Dynamic numeric fields cross the aggregate boundary as IEEE f64.
			// Do not forward an internal integer/f32 representation across it.
			return candidate->vt == type::nil || candidate->vt == type::i1 || candidate->vt == type::f64;
		}

		bool is_scalar_table_key(const constant* key) {
			return key->vt == type::i1 || key->vt == type::str || is_integer_data(key->vt) || is_floating_point_data(key->vt);
		}

		bool same_table_key(const constant* left, const constant* right) {
			auto lhs = left->to_any();
			auto rhs = right->to_any();
			if (lhs.value == rhs.value)
				return true;
			if (lhs.is_str() && rhs.is_str())
				return string_value_equals(lhs.as_str(), rhs.as_str());
			return canonicalize_zero_bits(lhs.value) == canonicalize_zero_bits(rhs.value);
		}

		bool constant_array_index(const constant* key, uint64_t& index) {
			double numeric;
			if (is_integer_data(key->vt)) {
				numeric = static_cast<double>(key->i);
			} else if (is_floating_point_data(key->vt)) {
				numeric = key->n;
			} else {
				return false;
			}

			if (!std::isfinite(numeric) || numeric < 0 || numeric != std::trunc(numeric) || numeric >= std::ldexp(1.0, std::numeric_limits<msize_t>::digits))
				return false;
			index = static_cast<uint64_t>(numeric);
			return true;
		}

		void refresh_users(const std::vector<insn*>& roots) {
			std::vector<insn*>        pending = roots;
			std::unordered_set<insn*> visited;
			while (!pending.empty()) {
				auto* current = pending.back();
				pending.pop_back();
				if (!current->parent || !visited.emplace(current).second)
					continue;
				current->update();
				current->for_each_user([&](insn* user, size_t) {
					pending.emplace_back(user);
					return false;
				});
			}
		}

		void replace_uses(insn* from, value* with, std::vector<insn*>& changed_users) {
			from->for_each_user([&](insn* user, size_t operand) {
				user->operands[operand].reset(with);
				changed_users.emplace_back(user);
				return false;
			});
		}

		struct tracked_field {
			constant* key         = nullptr;
			uint64_t  array_index = 0;
		};

		struct symbolic_value;

		struct operation_plan {
			ref<insn>              instruction;
			size_t                 field = no_field;
			std::vector<ref<insn>> value_extracts;
			std::vector<ref<insn>> status_extracts;
			symbolic_value*        replacement = nullptr;
		};

		struct candidate_plan {
			procedure*                        proc = nullptr;
			ref<insn>                         allocation;
			bool                              is_array     = false;
			uint64_t                          array_length = 0;
			std::vector<ref<insn>>            aliases;
			std::unordered_set<insn*>         alias_set;
			std::vector<ref<insn>>            aggregate_status_extracts;
			std::vector<ref<insn>>            lifetime_markers;
			std::vector<tracked_field>        fields;
			std::vector<operation_plan>       operations;
			std::unordered_map<insn*, size_t> operation_indices;
		};

		bool alias_result_type(const candidate_plan& plan, const insn* instruction) {
			return instruction->vt == type::any || instruction->vt == plan.allocation->vt;
		}

		bool discover_aliases(candidate_plan& plan) {
			plan.alias_set.emplace(plan.allocation.get());
			plan.aliases.emplace_back(plan.allocation);

			bool changed;
			do {
				changed = false;
				for (auto& block : plan.proc->basic_blocks) {
					for (auto* instruction : block->insns()) {
						if (plan.alias_set.contains(instruction) || !alias_result_type(plan, instruction))
							continue;

						bool forwards = false;
						if (instruction->is<assume_cast>() || instruction->is<move>() || instruction->is<erase_type>()) {
							forwards = instruction->operands[0]->is<insn>() && plan.alias_set.contains(instruction->operands[0]->as<insn>());
						} else if (instruction->is<extract>()) {
							auto index = instruction->operands[1]->as<constant>()->i32;
							forwards   = index == 0 && instruction->operands[0]->is<insn>() && plan.alias_set.contains(instruction->operands[0]->as<insn>());
						} else if (instruction->is<phi>()) {
							bool touches = false;
							forwards     = !instruction->operands.empty();
							for (const auto& operand : instruction->operands) {
								touches |= operand.get() != instruction && operand->is<insn>() && plan.alias_set.contains(operand->as<insn>());
								forwards &= operand.get() == instruction || (operand->is<insn>() && plan.alias_set.contains(operand->as<insn>()));
							}
							forwards &= touches;
						} else if (instruction->is<select>()) {
							forwards = instruction->operands[1]->is<insn>() && instruction->operands[2]->is<insn>() &&
										  plan.alias_set.contains(instruction->operands[1]->as<insn>()) && plan.alias_set.contains(instruction->operands[2]->as<insn>());
						}

						if (!forwards)
							continue;
						if (plan.aliases.size() == max_aliases)
							return false;
						plan.alias_set.emplace(instruction);
						plan.aliases.emplace_back(make_ref(instruction));
						changed = true;
					}
				}
			} while (changed);
			return true;
		}

		bool is_forwarding_use(const candidate_plan& plan, insn* user, size_t operand) {
			if (!plan.alias_set.contains(user))
				return false;
			if (user->is<assume_cast>() || user->is<move>() || user->is<erase_type>())
				return operand == 0;
			if (user->is<extract>())
				return operand == 0 && user->operands[1]->as<constant>()->i32 == 0;
			if (user->is<phi>())
				return true;
			if (user->is<select>())
				return operand == 1 || operand == 2;
			return false;
		}

		bool collect_aggregate_uses(candidate_plan& plan, std::unordered_set<insn*>& field_operations) {
			std::unordered_set<insn*> aggregate_checks;
			std::unordered_set<insn*> lifetime_markers;
			size_t                    use_count = 0;

			for (const auto& alias : plan.aliases) {
				bool rejected = alias->for_each_user([&](insn* user, size_t operand) {
					if (++use_count > max_candidate_uses)
						return true;
					if (is_forwarding_use(plan, user, operand))
						return false;
					if ((user->is<field_get>() || user->is<field_set>()) && operand == 1) {
						field_operations.emplace(user);
						return field_operations.size() > max_field_operations;
					}
					if (user->is<keep_alive>() && operand == 0) {
						lifetime_markers.emplace(user);
						return false;
					}
					if (user->is<extract>() && operand == 0 && user->operands[1]->as<constant>()->i32 == 1) {
						aggregate_checks.emplace(user);
						return false;
					}
					return true;
				});
				if (rejected)
					return false;
			}

			for (auto& block : plan.proc->basic_blocks) {
				for (auto* instruction : block->insns()) {
					if (aggregate_checks.contains(instruction))
						plan.aggregate_status_extracts.emplace_back(make_ref(instruction));
					if (lifetime_markers.contains(instruction))
						plan.lifetime_markers.emplace_back(make_ref(instruction));
				}
			}
			return true;
		}

		size_t find_or_add_array_field(candidate_plan& plan, uint64_t index) {
			for (size_t field = 0; field != plan.fields.size(); ++field) {
				if (plan.fields[field].array_index == index)
					return field;
			}
			if (plan.fields.size() == max_tracked_fields)
				return no_field;
			plan.fields.push_back({nullptr, index});
			return plan.fields.size() - 1;
		}

		size_t find_or_add_table_field(candidate_plan& plan, constant* key) {
			for (size_t field = 0; field != plan.fields.size(); ++field) {
				if (same_table_key(plan.fields[field].key, key))
					return field;
			}
			if (plan.fields.size() == max_tracked_fields)
				return no_field;
			plan.fields.push_back({key, 0});
			return plan.fields.size() - 1;
		}

		bool collect_result_extracts(operation_plan& operation) {
			size_t uses     = 0;
			bool   rejected = operation.instruction->for_each_user([&](insn* user, size_t operand) {
				if (++uses > max_candidate_uses || operand != 0 || !user->is<extract>())
					return true;
				auto index = user->operands[1]->as<constant>()->i32;
				if (index == 0)
					operation.value_extracts.emplace_back(make_ref(user));
				else
					operation.status_extracts.emplace_back(make_ref(user));
				return false;
			});
			return !rejected;
		}

		bool collect_operations(candidate_plan& plan, const std::unordered_set<insn*>& field_operations) {
			for (auto& block : plan.proc->basic_blocks) {
				for (auto* instruction : block->insns()) {
					if (!field_operations.contains(instruction))
						continue;
					if (instruction->parent == plan.allocation->parent) {
						if (!precedes_in_block(plan.allocation.get(), instruction))
							return false;
					} else if (!plan.allocation->parent->dom(instruction->parent)) {
						return false;
					}

					operation_plan operation{make_ref(instruction)};
					if (!collect_result_extracts(operation))
						return false;
					if (instruction->is<field_set>() && !is_primitive_scalar(instruction->operands[3].get()))
						return false;

					auto* key_value = instruction->operands[2].get();
					if (!key_value->is<constant>())
						return false;
					auto* key = key_value->as<constant>();
					if (plan.is_array) {
						uint64_t index;
						if (!constant_array_index(key, index))
							return false;
						if (index >= plan.array_length) {
							if (instruction->is<field_set>())
								return false;
							operation.field = no_field;
						} else {
							operation.field = find_or_add_array_field(plan, index);
							if (operation.field == no_field)
								return false;
						}
					} else {
						if (!is_scalar_table_key(key))
							return false;
						operation.field = find_or_add_table_field(plan, key);
						if (operation.field == no_field)
							return false;
					}

					plan.operation_indices.emplace(instruction, plan.operations.size());
					plan.operations.emplace_back(std::move(operation));
				}
			}
			return true;
		}

		bool build_candidate_plan(procedure* proc, insn* allocation, candidate_plan& plan) {
			plan.proc       = proc;
			plan.allocation = make_ref(allocation);
			plan.is_array   = allocation->is<array_new>();

			auto* size_value = allocation->operands[0].get();
			if (!size_value->is<constant>() || size_value->vt != type::i32 || size_value->as<constant>()->i32 < 0)
				return false;
			if (plan.is_array)
				plan.array_length = static_cast<uint32_t>(size_value->as<constant>()->i32);

			if (!discover_aliases(plan))
				return false;
			for (const auto& alias : plan.aliases) {
				if (alias->parent == allocation->parent) {
					if (alias.get() != allocation && !precedes_in_block(allocation, alias.get()))
						return false;
				} else if (!allocation->parent->dom(alias->parent)) {
					return false;
				}
			}

			std::unordered_set<insn*> field_operations;
			if (!collect_aggregate_uses(plan, field_operations))
				return false;
			return collect_operations(plan, field_operations);
		}

		struct symbolic_value {
			ref<>                        concrete;
			type                         vt          = type::nil;
			basic_block*                 merge_block = nullptr;
			std::vector<symbolic_value*> incoming;
			ref<>                        materialized;
		};

		struct block_value_state {
			enum state_kind : uint8_t {
				unseen,
				visiting,
				complete,
				failed,
			} state = unseen;
			std::vector<symbolic_value*> exit_values;
		};

		struct scalar_dataflow {
			candidate_plan&                                                      plan;
			std::vector<std::unique_ptr<symbolic_value>>                         symbols;
			std::unordered_map<value*, symbolic_value*>                          concrete_symbols;
			std::unordered_map<basic_block*, std::unique_ptr<block_value_state>> blocks;
			symbolic_value*                                                      nil_value   = nullptr;
			size_t                                                               merge_count = 0;

			symbolic_value* concrete(value* source) {
				if (auto found = concrete_symbols.find(source); found != concrete_symbols.end())
					return found->second;
				auto node      = std::make_unique<symbolic_value>();
				node->concrete = make_ref(source);
				node->vt       = source->vt;
				auto* result   = node.get();
				symbols.emplace_back(std::move(node));
				concrete_symbols.emplace(source, result);
				return result;
			}

			symbolic_value* merge(basic_block* block, std::vector<symbolic_value*> incoming) {
				auto* first = incoming.front();
				bool  same  = true;
				for (auto* value : incoming)
					same &= value == first;
				if (same)
					return first;
				for (auto* value : incoming) {
					if (value->vt != first->vt)
						return nullptr;
				}
				if (merge_count == max_scalar_phis)
					return nullptr;

				auto node         = std::make_unique<symbolic_value>();
				node->vt          = first->vt;
				node->merge_block = block;
				node->incoming    = std::move(incoming);
				auto* result      = node.get();
				symbols.emplace_back(std::move(node));
				++merge_count;
				return result;
			}

			bool apply_operations(basic_block* block, std::vector<symbolic_value*>& values) {
				bool active = block != plan.allocation->parent;
				for (auto* instruction : block->insns()) {
					if (instruction == plan.allocation.get()) {
						active = true;
						continue;
					}
					auto operation = plan.operation_indices.find(instruction);
					if (operation == plan.operation_indices.end())
						continue;
					if (!active)
						return false;

					auto& current = plan.operations[operation->second];
					if (instruction->is<field_set>()) {
						LI_ASSERT(current.field != no_field);
						values[current.field] = concrete(instruction->operands[3].get());
					} else {
						current.replacement = current.field == no_field ? nil_value : values[current.field];
					}
				}
				return true;
			}

			bool compute_exit(basic_block* block) {
				auto found = blocks.find(block);
				if (found == blocks.end()) {
					if (blocks.size() == max_dataflow_blocks)
						return false;
					found = blocks.emplace(block, std::make_unique<block_value_state>()).first;
				}
				auto* record = found->second.get();
				if (record->state == block_value_state::complete)
					return true;
				if (record->state == block_value_state::visiting || record->state == block_value_state::failed)
					return false;
				record->state = block_value_state::visiting;

				std::vector<symbolic_value*> values(plan.fields.size(), nil_value);
				if (block != plan.allocation->parent) {
					if (!plan.allocation->parent->dom(block) || block->predecessors.empty()) {
						record->state = block_value_state::failed;
						return false;
					}

					std::vector<const std::vector<symbolic_value*>*> incoming_states;
					incoming_states.reserve(block->predecessors.size());
					for (auto* predecessor : block->predecessors) {
						if (predecessor != plan.allocation->parent && !plan.allocation->parent->dom(predecessor)) {
							record->state = block_value_state::failed;
							return false;
						}
						if (!compute_exit(predecessor)) {
							record->state = block_value_state::failed;
							return false;
						}
						incoming_states.emplace_back(&blocks.at(predecessor)->exit_values);
					}

					for (size_t field = 0; field != plan.fields.size(); ++field) {
						std::vector<symbolic_value*> incoming;
						incoming.reserve(incoming_states.size());
						for (auto* state : incoming_states)
							incoming.emplace_back((*state)[field]);
						values[field] = merge(block, std::move(incoming));
						if (!values[field]) {
							record->state = block_value_state::failed;
							return false;
						}
					}
				}

				if (!apply_operations(block, values)) {
					record->state = block_value_state::failed;
					return false;
				}
				record->exit_values = std::move(values);
				record->state       = block_value_state::complete;
				return true;
			}

			bool analyze() {
				auto nil  = std::make_unique<symbolic_value>();
				nil_value = nil.get();
				symbols.emplace_back(std::move(nil));
				for (auto& operation : plan.operations) {
					if (!compute_exit(operation.instruction->parent))
						return false;
				}
				for (const auto& operation : plan.operations) {
					if (operation.instruction->is<field_get>() && !operation.replacement)
						return false;
				}
				return true;
			}

			value* materialize(symbolic_value* value) {
				if (value->concrete)
					return value->concrete.get();
				if (value->materialized)
					return value->materialized.get();
				if (!value->merge_block) {
					value->materialized = plan.proc->add_const(constant(any(nil)));
					return value->materialized.get();
				}

				builder b{value->merge_block};
				b.current_bc = value->merge_block->bc_begin;
				auto joined  = b.create<phi>(plan.proc);
				for (auto* incoming : value->incoming)
					joined->operands.emplace_back(make_use(materialize(incoming)));
				value->merge_block->insert(value->merge_block->end_phi(), joined);
				joined->update();
				value->materialized = joined;
				return joined.get();
			}
		};

		void erase_instruction_group(std::vector<ref<insn>>& instructions) {
			for (auto& instruction : instructions) {
				if (instruction->parent)
					instruction->erase();
			}
		}

		void commit_candidate(candidate_plan& plan, scalar_dataflow& dataflow) {
			auto               true_value = plan.proc->add_const(constant(true));
			auto               nil_value  = plan.proc->add_const(constant(any(nil)));
			std::vector<insn*> changed_users;

			for (auto& operation : plan.operations) {
				value* payload = operation.instruction->is<field_get>() ? dataflow.materialize(operation.replacement) : nil_value.get();
				for (auto& extraction : operation.value_extracts)
					replace_uses(extraction.get(), payload, changed_users);
				for (auto& extraction : operation.status_extracts)
					replace_uses(extraction.get(), true_value.get(), changed_users);
			}
			for (auto& extraction : plan.aggregate_status_extracts)
				replace_uses(extraction.get(), true_value.get(), changed_users);
			refresh_users(changed_users);

			for (auto& operation : plan.operations) {
				erase_instruction_group(operation.value_extracts);
				erase_instruction_group(operation.status_extracts);
				LI_ASSERT(operation.instruction->use_count() == 0);
				operation.instruction->erase();
			}
			erase_instruction_group(plan.aggregate_status_extracts);
			erase_instruction_group(plan.lifetime_markers);

			// Forwarding aliases may form PHI cycles. Drop their operands as a group
			// only after every observable aggregate use has been removed.
			for (auto& alias : plan.aliases)
				alias->operands.clear();
			for (auto& alias : plan.aliases) {
				LI_ASSERT(alias->use_count() == 0);
				alias->erase();
			}
		}
	}

	size_t scalar_replace(procedure* proc) {
		std::vector<ref<insn>> allocations;
		for (auto& block : proc->basic_blocks) {
			for (auto* instruction : block->insns()) {
				if (instruction->is<array_new>() || instruction->is<table_new>())
					allocations.emplace_back(make_ref(instruction));
			}
		}

		size_t transformed = 0;
		for (auto& allocation : allocations) {
			if (!allocation->parent)
				continue;
			candidate_plan plan;
			if (!build_candidate_plan(proc, allocation.get(), plan))
				continue;
			scalar_dataflow dataflow{plan};
			if (!dataflow.analyze())
				continue;
			commit_candidate(plan, dataflow);
			++transformed;
		}
		return transformed;
	}
};
