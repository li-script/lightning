#include <ir/proc.hpp>
#include <ir/verify.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace li::ir {
	namespace {
		struct verified_block {
			basic_block*       block = nullptr;
			std::vector<insn*> instructions;
		};

		class verifier {
		  public:
			explicit verifier(procedure& proc) : proc(proc) {}

			std::optional<verification_error> run() {
				if (!check(!proc.basic_blocks.empty(), "procedure has no entry block"))
					return error;
				if (!collect_owners() || !collect_instructions() || !verify_values_and_shapes() || !verify_cfg() || !verify_dominance() || !verify_ownership())
					return error;
				return std::nullopt;
			}

		  private:
			procedure&                              proc;
			std::optional<verification_error>       error;
			std::unordered_set<const basic_block*>  all_blocks;
			std::unordered_set<const constant*>     all_constants;
			std::unordered_set<const insn*>         all_instructions;
			std::unordered_map<const insn*, size_t> instruction_positions;
			std::unordered_map<value*, value*>      ownership_roots;
			std::vector<verified_block>             blocks;

			bool fail(std::string message, const basic_block* block = nullptr, const insn* instruction = nullptr) {
				if (!error)
					error = verification_error{std::move(message), block, instruction};
				return false;
			}

			bool check(bool condition, const char* message, const basic_block* block = nullptr, const insn* instruction = nullptr) {
				return condition || fail(message, block, instruction);
			}

			static bool is_instruction_value_type(type vt) { return static_cast<int32_t>(vt) <= static_cast<int32_t>(type::f32); }
			static bool is_operand_value_type(type vt) { return is_instruction_value_type(vt) && vt != type::none; }
			static bool is_integer_type(type vt) { return vt == type::i1 || is_integer_data(vt); }
			static bool is_reference_type(type vt) { return vt == type::any || is_gc_data(vt); }

			static bool is_metadata_operand(opcode opc, size_t index) {
				switch (opc) {
					case opcode::load_local:
					case opcode::store_local:
					case opcode::osr_load:
						return index == 0;
					case opcode::field_get:
					case opcode::field_set:
					case opcode::unop:
					case opcode::binop:
					case opcode::compare:
						return index == 0;
					case opcode::struct_array_get:
						return index == 2;
					case opcode::struct_array_class_test:
						return index == 1;
					case opcode::struct_array_field_load:
						return index >= 2;
					case opcode::iter_next:
					case opcode::extract:
					case opcode::uval_get:
					case opcode::uval_set:
					case opcode::assume_cast:
					case opcode::test_type:
						return index == 1;
					case opcode::ccall:
						return index < 2;
					case opcode::jmp:
						return index == 0;
					case opcode::jcc:
						return index == 1 || index == 2;
					case opcode::deopt:
						return index == 0 || (index & 1);
					default:
						return false;
				}
			}

			static bool produces_value(opcode opc) {
				switch (opc) {
					case opcode::store_local:
					case opcode::safepoint:
					case opcode::keep_alive:
					case opcode::release:
					case opcode::jmp:
					case opcode::jcc:
					case opcode::ret:
					case opcode::unreachable:
					case opcode::deopt:
						return false;
					default:
						return true;
				}
			}

			bool collect_owners() {
				std::unordered_set<msize_t> block_uids;
				for (auto& owner : proc.basic_blocks) {
					if (!check(owner != nullptr, "null block owner"))
						return false;
					auto* block = owner.get();
					if (!check(all_blocks.emplace(block).second, "block is owned more than once", block) ||
						 !check(block_uids.emplace(block->uid).second, "duplicate block identifier", block) ||
						 !check(block->proc == &proc, "block belongs to another procedure", block))
						return false;
				}

				for (auto& [key, owner] : proc.consts) {
					if (!check(owner != nullptr, "null constant owner") || !check(all_constants.emplace(owner.get()).second, "constant is owned more than once") ||
						 !check(key == *owner, "constant-pool key does not match its value") ||
						 !check(static_cast<int32_t>(owner->vt) <= static_cast<int32_t>(type::dty) && owner->vt != type::none && owner->vt != type::any,
							  "constant has invalid type"))
						return false;
					if (owner->vt == type::bb && !check(owner->bb != nullptr && all_blocks.contains(owner->bb), "block constant refers outside its procedure"))
						return false;
				}
				return true;
			}

			bool collect_instructions() {
				blocks.reserve(proc.basic_blocks.size());
				for (auto& owner : proc.basic_blocks) {
					auto* block = owner.get();
					if (!check(block->insn_list_head.parent == nullptr, "instruction-list sentinel has a parent", block) ||
						 !check(block->insn_list_head.next != nullptr && block->insn_list_head.prev != nullptr, "null instruction-list link", block))
						return false;

					verified_block                  result{block};
					std::unordered_set<const insn*> local_instructions;
					insn*                           previous = &block->insn_list_head;
					insn*                           current  = block->insn_list_head.next;
					while (current != &block->insn_list_head) {
						if (!check(current != nullptr, "null instruction-list link", block) ||
							 !check(local_instructions.emplace(current).second, "instruction-list cycle does not reach its sentinel", block, current) ||
							 !check(all_instructions.emplace(current).second, "instruction is owned by multiple blocks", block, current) ||
							 !check(current->parent == block, "instruction parent does not match its owner", block, current) ||
							 !check(current->prev == previous && previous->next == current, "broken previous instruction link", block, current) ||
							 !check(current->next != nullptr, "null next instruction link", block, current))
							return false;
						instruction_positions.emplace(current, result.instructions.size());
						result.instructions.emplace_back(current);
						previous = current;
						current  = current->next;
					}
					if (!check(previous == block->insn_list_head.prev, "instruction-list tail does not match its sentinel", block) ||
						 !check(block->insn_list_head.prev->next == &block->insn_list_head, "broken tail instruction link", block) ||
						 !check(block->insn_list_head.next->prev == &block->insn_list_head, "broken head instruction link", block) ||
						 !check(!result.instructions.empty(), "block has no instructions", block))
						return false;

					bool seen_non_phi = false;
					for (size_t index = 0; index != result.instructions.size(); ++index) {
						auto* instruction = result.instructions[index];
						if (instruction->is<phi>()) {
							if (!check(!seen_non_phi, "phi appears after a non-phi instruction", block, instruction) ||
								 !check(instruction->operands.size() == block->predecessors.size(), "phi operand count does not match predecessor count", block,
									  instruction))
								return false;
						} else {
							seen_non_phi = true;
						}
						if (instruction->is_terminator() &&
							 !check(index + 1 == result.instructions.size(), "terminator is not the last instruction", block, instruction))
							return false;
					}
					if (!check(result.instructions.back()->is_terminator(), "block is not terminated", block, result.instructions.back()))
						return false;
					blocks.emplace_back(std::move(result));
				}
				return true;
			}

			bool expect_arity(const insn* instruction, size_t count) {
				return check(instruction->operands.size() == count, "wrong operand count", instruction->parent, instruction);
			}
			bool expect_min_arity(const insn* instruction, size_t count) {
				return check(instruction->operands.size() >= count, "too few operands", instruction->parent, instruction);
			}
			const constant* expect_constant(const insn* instruction, size_t index, type expected) {
				if (index >= instruction->operands.size()) {
					fail("missing constant operand", instruction->parent, instruction);
					return nullptr;
				}
				auto* operand = instruction->operands[index].get();
				if (!check(operand->is<constant>(), "expected constant operand", instruction->parent, instruction))
					return nullptr;
				auto* result = operand->as<constant>();
				if (!check(result->vt == expected, "constant operand has wrong type", instruction->parent, instruction))
					return nullptr;
				return result;
			}
			bool expect_type(const insn* instruction, size_t index, type expected) {
				return check(index < instruction->operands.size() && instruction->operands[index]->vt == expected, "operand has wrong type", instruction->parent,
					 instruction);
			}

			bool verify_instruction_shape(insn* instruction) {
				switch (instruction->opc) {
					case opcode::invalid:
						return fail("invalid opcode", instruction->parent, instruction);
					case opcode::load_local:
						return expect_arity(instruction, 1) && expect_constant(instruction, 0, type::i32);
					case opcode::store_local:
						return expect_arity(instruction, 2) && expect_constant(instruction, 0, type::i32);
					case opcode::array_new:
					case opcode::table_new:
						return expect_arity(instruction, 1) && expect_type(instruction, 0, type::i32);
					case opcode::field_get:
						return expect_arity(instruction, 3) && expect_constant(instruction, 0, type::i1);
					case opcode::field_set:
						return expect_arity(instruction, 4);
					case opcode::struct_array_get:
						return expect_arity(instruction, 3) && expect_type(instruction, 0, type::tarr) && expect_constant(instruction, 2, type::vcl);
					case opcode::struct_array_class_test:
						return expect_arity(instruction, 2) && expect_type(instruction, 0, type::tarr) && expect_constant(instruction, 1, type::vcl);
					case opcode::struct_array_bounds_test:
						return expect_arity(instruction, 2) && expect_type(instruction, 0, type::tarr) && expect_type(instruction, 1, type::i32);
					case opcode::struct_array_field_load:
						return expect_arity(instruction, 5) && expect_type(instruction, 0, type::tarr) && expect_type(instruction, 1, type::i32) &&
								 expect_constant(instruction, 2, type::i32) && expect_constant(instruction, 3, type::i32) && expect_constant(instruction, 4, type::dty);
					case opcode::class_new:
						return expect_arity(instruction, 1);
					case opcode::class_is:
						return expect_arity(instruction, 2);
					case opcode::iter_next:
						return expect_arity(instruction, 2) && expect_constant(instruction, 1, type::i32);
					case opcode::unop: {
						if (!expect_arity(instruction, 2))
							return false;
						auto* operation_value = expect_constant(instruction, 0, type::vmopr);
						return operation_value && check(operation_value->vmopr == operation::ANEG || operation_value->vmopr == operation::LNOT,
																"invalid unary operation", instruction->parent, instruction);
					}
					case opcode::binop: {
						if (!expect_arity(instruction, 3))
							return false;
						auto* operation_value = expect_constant(instruction, 0, type::vmopr);
						return operation_value && check(operation::AADD <= operation_value->vmopr && operation_value->vmopr <= operation::APOW,
																"invalid binary operation", instruction->parent, instruction);
					}
					case opcode::bool_and:
					case opcode::bool_or:
					case opcode::bool_xor:
						return expect_arity(instruction, 2) &&
								 check(is_integer_type(instruction->operands[0]->vt) && instruction->operands[0]->vt == instruction->operands[1]->vt,
									  "boolean operands must have the same integer type", instruction->parent, instruction);
					case opcode::safepoint:
					case opcode::va_count:
						return expect_arity(instruction, 0);
					case opcode::va_get:
						return expect_arity(instruction, 1);
					case opcode::extract: {
						if (!expect_arity(instruction, 2))
							return false;
						auto* index = expect_constant(instruction, 1, type::i32);
						return index && check(index->i32 == 0 || index->i32 == 1, "extract index is out of range", instruction->parent, instruction);
					}
					case opcode::uval_get:
						return expect_arity(instruction, 2) && expect_type(instruction, 0, type::fn) && expect_type(instruction, 1, type::i32);
					case opcode::uval_set:
						return expect_arity(instruction, 3) && expect_type(instruction, 0, type::fn) && expect_type(instruction, 1, type::i32);
					case opcode::assume_cast: {
						if (!expect_arity(instruction, 2))
							return false;
						auto* target = expect_constant(instruction, 1, type::dty);
						return target && check(is_operand_value_type(target->dty), "invalid assumed type", instruction->parent, instruction);
					}
					case opcode::coerce_bool:
					case opcode::move:
					case opcode::erase_type:
					case opcode::keep_alive:
					case opcode::retain:
					case opcode::release:
						return expect_arity(instruction, 1);
					case opcode::test_type: {
						if (!expect_arity(instruction, 2))
							return false;
						auto* target = expect_constant(instruction, 1, type::vty);
						return target && check(target->vty <= type_number, "invalid tested value type", instruction->parent, instruction);
					}
					case opcode::compare: {
						if (!expect_arity(instruction, 3))
							return false;
						auto* operation_value = expect_constant(instruction, 0, type::vmopr);
						return operation_value && check(operation::CEQ <= operation_value->vmopr && operation_value->vmopr <= operation::CLE,
																"invalid comparison operation", instruction->parent, instruction);
					}
					case opcode::select:
						return expect_arity(instruction, 3) && expect_type(instruction, 0, type::i1);
					case opcode::phi:
						return true;
					case opcode::set_exception:
						return expect_arity(instruction, 1);
					case opcode::get_exception:
						return expect_arity(instruction, 0);
					case opcode::ccall: {
						if (!expect_min_arity(instruction, 2))
							return false;
						auto* target_value = expect_constant(instruction, 0, type::nfni);
						auto* index_value  = expect_constant(instruction, 1, type::i32);
						if (!target_value || !index_value || !check(target_value->nfni != nullptr, "null native-call target", instruction->parent, instruction))
							return false;
						auto overloads = target_value->nfni->get_overloads();
						if (!check(index_value->i32 >= 0 && size_t(index_value->i32) < overloads.size(), "native-call overload is out of range", instruction->parent,
								  instruction))
							return false;
						auto& overload = overloads[size_t(index_value->i32)];
						if (!expect_arity(instruction, overload.args.size() + 2))
							return false;
						for (size_t n = 0; n != overload.args.size(); ++n) {
							auto actual   = instruction->operands[n + 2]->vt;
							auto expected = overload.args[n];
							if (!check(actual == expected || (expected == type::any && is_operand_value_type(actual)), "native-call argument has wrong type",
									  instruction->parent, instruction))
								return false;
						}
						return true;
					}
					case opcode::vcall:
						return expect_min_arity(instruction, 2);
					case opcode::osr_load:
						return expect_arity(instruction, 1) && expect_constant(instruction, 0, type::i32);
					case opcode::jmp:
						return expect_arity(instruction, 1) && expect_constant(instruction, 0, type::bb);
					case opcode::jcc:
						return expect_arity(instruction, 3) && expect_type(instruction, 0, type::i1) && expect_constant(instruction, 1, type::bb) &&
								 expect_constant(instruction, 2, type::bb);
					case opcode::ret:
						return expect_arity(instruction, 1);
					case opcode::unreachable:
						return expect_arity(instruction, 0);
					case opcode::deopt:
						if (!check(!instruction->operands.empty() && (instruction->operands.size() & 1),
								  "deopt operand list must contain an instruction position and slot/value pairs", instruction->parent, instruction) ||
							 !expect_constant(instruction, 0, type::i32))
							return false;
						for (size_t index = 1; index < instruction->operands.size(); index += 2)
							if (!expect_constant(instruction, index, type::i32))
								return false;
						return true;
					default:
						return fail("unknown opcode", instruction->parent, instruction);
				}
			}

			bool verify_effect_model(const insn* instruction) {
				constexpr uint32_t known_effects = effect_read | effect_write | effect_alloc | effect_may_throw | effect_may_call_user | effect_release_may_destroy;
				if (!check((instruction->effects & ~known_effects) == 0, "instruction carries unknown effect flags", instruction->parent, instruction))
					return false;
				if (instruction->has_effect(effect_may_call_user) &&
					 !check(instruction->has_effect(effect_may_throw) && instruction->has_effect(effect_release_may_destroy),
						  "effect_may_call_user requires effect_may_throw and effect_release_may_destroy", instruction->parent, instruction))
					return false;
				return true;
			}

			bool verify_values_and_shapes() {
				std::unordered_map<const value*, size_t> observed_uses;
				for (auto& block : blocks) {
					for (auto* instruction : block.instructions) {
						if (!check(is_instruction_value_type(instruction->vt), "instruction has an invalid result type", block.block, instruction))
							return false;
						for (size_t operand_index = 0; operand_index != instruction->operands.size(); ++operand_index) {
							auto* operand = instruction->operands[operand_index].get();
							if (!check(operand != nullptr, "null operand", block.block, instruction) ||
								 !check(is_metadata_operand(instruction->opc, operand_index) || is_operand_value_type(operand->vt), "operand has an invalid value type",
									  block.block, instruction))
								return false;
							if (operand->is<insn>()) {
								auto* definition = operand->as<insn>();
								if (!check(definition->parent != nullptr, "operand instruction is orphaned", nullptr, definition) ||
									 !check(all_instructions.contains(definition), "operand instruction is not owned by this procedure", definition->parent, definition))
									return false;
							} else if (operand->is<constant>()) {
								if (!check(
										  all_constants.contains(operand->as<constant>()), "operand constant is not owned by this procedure", block.block, instruction))
									return false;
							} else {
								return fail("operand is neither an instruction nor a constant", block.block, instruction);
							}
							++observed_uses[operand];
						}
						if (!verify_instruction_shape(instruction) || !verify_effect_model(instruction))
							return false;
					}
				}

				for (auto& block : blocks) {
					for (auto* instruction : block.instructions) {
						auto old_effects = instruction->effects;
						instruction->update();
						if (!check((instruction->effects & old_effects) == old_effects, "instruction update removed effect flags", block.block, instruction) ||
							 !check(is_instruction_value_type(instruction->vt), "instruction update produced an invalid result type", block.block, instruction) ||
							 !verify_effect_model(instruction))
							return false;
					}
				}

				for (auto* instruction : all_instructions) {
					auto count = observed_uses.find(instruction);
					if (!check(instruction->use_count() == (count == observed_uses.end() ? 0 : count->second), "instruction use count does not match its operands",
							  instruction->parent, instruction))
						return false;
				}
				for (auto* constant : all_constants) {
					auto count = observed_uses.find(constant);
					if (!check(constant->use_count() == (count == observed_uses.end() ? 0 : count->second), "constant use count does not match its operands"))
						return false;
				}
				return true;
			}

			bool verify_cfg() {
				struct indexed_edge {
					size_t from;
					size_t to;
					bool   operator<(const indexed_edge& other) const { return from < other.from || (from == other.from && to < other.to); }
					bool   operator==(const indexed_edge&) const = default;
				};

				std::unordered_map<const basic_block*, size_t> indices;
				indices.reserve(blocks.size());
				for (size_t index = 0; index != blocks.size(); ++index)
					indices.emplace(blocks[index].block, index);

				std::vector<indexed_edge> successor_edges;
				std::vector<indexed_edge> predecessor_edges;
				for (size_t index = 0; index != blocks.size(); ++index) {
					auto* current = blocks[index].block;
					for (auto* successor : current->successors) {
						if (!check(successor != nullptr && all_blocks.contains(successor), "successor refers outside the procedure", current,
								  blocks[index].instructions.back()))
							return false;
						successor_edges.push_back({index, indices.at(successor)});
					}
					for (auto* predecessor : current->predecessors) {
						if (!check(predecessor != nullptr && all_blocks.contains(predecessor), "predecessor refers outside the procedure", current,
								  blocks[index].instructions.back()))
							return false;
						predecessor_edges.push_back({indices.at(predecessor), index});
					}

					auto* terminator = blocks[index].instructions.back();
					if (terminator->is<jmp>()) {
						auto* target = terminator->operands[0]->as<constant>()->bb;
						if (!check(
								  current->successors.size() == 1 && current->successors[0] == target, "jmp target does not match successor list", current, terminator))
							return false;
					} else if (terminator->is<jcc>()) {
						auto* true_target  = terminator->operands[1]->as<constant>()->bb;
						auto* false_target = terminator->operands[2]->as<constant>()->bb;
						if (!check(current->successors.size() == 2 && current->successors[0] == true_target && current->successors[1] == false_target,
								  "jcc targets do not match successor order", current, terminator))
							return false;
					} else if (!check(terminator->is_proc_terminator(), "unknown block terminator", current, terminator) ||
								  !check(current->successors.empty(), "procedure terminator has successors", current, terminator)) {
						return false;
					}
				}

				std::sort(successor_edges.begin(), successor_edges.end());
				std::sort(predecessor_edges.begin(), predecessor_edges.end());
				if (successor_edges != predecessor_edges) {
					size_t source = 0;
					if (!successor_edges.empty())
						source = successor_edges.front().from;
					else if (!predecessor_edges.empty())
						source = predecessor_edges.front().from;
					return fail("successor/predecessor edge multiplicity differs", blocks[source].block, blocks[source].instructions.back());
				}

				auto* entry = proc.get_entry();
				for (auto& block : blocks) {
					if (block.block != entry &&
						 !check(!block.block->predecessors.empty(), "non-entry block has no predecessors", block.block, block.instructions.back()))
						return false;
				}

				std::unordered_set<const basic_block*> reachable;
				std::vector<const basic_block*>        worklist{entry};
				while (!worklist.empty()) {
					auto* block = worklist.back();
					worklist.pop_back();
					if (!reachable.emplace(block).second)
						continue;
					worklist.insert(worklist.end(), block->successors.begin(), block->successors.end());
				}
				if (reachable.size() != all_blocks.size()) {
					for (auto& block : blocks)
						if (!reachable.contains(block.block))
							return fail("procedure contains blocks unreachable from entry", block.block, block.instructions.back());
				}
				return true;
			}

			bool verify_dominance() {
				const size_t                                   no_index = std::numeric_limits<size_t>::max();
				std::unordered_map<const basic_block*, size_t> block_index;
				for (size_t index = 0; index != blocks.size(); ++index)
					block_index.emplace(blocks[index].block, index);

				std::vector<size_t> dfs_number(blocks.size(), 0);
				std::vector<size_t> vertex(1, no_index);
				std::vector<size_t> parent(1, 0);
				struct dfs_frame {
					size_t block;
					size_t next_successor;
				};
				auto entry_index        = block_index.at(proc.get_entry());
				dfs_number[entry_index] = 1;
				vertex.push_back(entry_index);
				parent.push_back(0);
				std::vector<dfs_frame> stack{{entry_index, 0}};
				while (!stack.empty()) {
					auto& frame = stack.back();
					auto* block = blocks[frame.block].block;
					if (frame.next_successor == block->successors.size()) {
						stack.pop_back();
						continue;
					}
					auto successor_index = block_index.at(block->successors[frame.next_successor++]);
					if (dfs_number[successor_index])
						continue;
					auto number                 = vertex.size();
					dfs_number[successor_index] = number;
					vertex.push_back(successor_index);
					parent.push_back(dfs_number[frame.block]);
					stack.push_back({successor_index, 0});
				}
				if (!check(vertex.size() == blocks.size() + 1, "procedure contains blocks unreachable from entry"))
					return false;

				const size_t                     count = blocks.size();
				std::vector<std::vector<size_t>> predecessors(count + 1);
				for (size_t number = 1; number <= count; ++number) {
					for (auto* predecessor : blocks[vertex[number]].block->predecessors)
						predecessors[number].push_back(dfs_number[block_index.at(predecessor)]);
				}

				std::vector<size_t>              semi(count + 1), idom(count + 1), ancestor(count + 1, 0), label(count + 1);
				std::vector<std::vector<size_t>> bucket(count + 1);
				for (size_t number = 1; number <= count; ++number)
					semi[number] = label[number] = number;

				auto compress = [&](auto& self, size_t node) -> void {
					if (ancestor[ancestor[node]] != 0) {
						self(self, ancestor[node]);
						if (semi[label[ancestor[node]]] < semi[label[node]])
							label[node] = label[ancestor[node]];
						ancestor[node] = ancestor[ancestor[node]];
					}
				};
				auto eval = [&](size_t node) {
					if (ancestor[node] == 0)
						return label[node];
					compress(compress, node);
					return semi[label[ancestor[node]]] < semi[label[node]] ? label[ancestor[node]] : label[node];
				};

				for (size_t number = count; number > 1; --number) {
					for (auto predecessor : predecessors[number])
						semi[number] = std::min(semi[number], semi[eval(predecessor)]);
					bucket[semi[number]].push_back(number);
					ancestor[number] = parent[number];
					for (auto pending : bucket[parent[number]]) {
						auto evaluated = eval(pending);
						idom[pending]  = semi[evaluated] < semi[pending] ? evaluated : parent[number];
					}
					bucket[parent[number]].clear();
				}
				for (size_t number = 2; number <= count; ++number)
					if (idom[number] != semi[number])
						idom[number] = idom[idom[number]];

				std::vector<std::vector<size_t>> children(count + 1);
				for (size_t number = 2; number <= count; ++number)
					children[idom[number]].push_back(number);
				std::vector<size_t> pre(count + 1), post(count + 1);
				struct tree_frame {
					size_t node;
					size_t next_child;
				};
				size_t                  clock = 0;
				std::vector<tree_frame> tree_stack{{1, 0}};
				pre[1] = ++clock;
				while (!tree_stack.empty()) {
					auto& frame = tree_stack.back();
					if (frame.next_child != children[frame.node].size()) {
						auto child = children[frame.node][frame.next_child++];
						pre[child] = ++clock;
						tree_stack.push_back({child, 0});
					} else {
						post[frame.node] = ++clock;
						tree_stack.pop_back();
					}
				}
				auto dominates = [&](const basic_block* definition, const basic_block* use) {
					auto definition_number = dfs_number[block_index.at(definition)];
					auto use_number        = dfs_number[block_index.at(use)];
					return pre[definition_number] <= pre[use_number] && post[use_number] <= post[definition_number];
				};

				for (auto& block : blocks) {
					for (auto* instruction : block.instructions) {
						for (size_t operand_index = 0; operand_index != instruction->operands.size(); ++operand_index) {
							auto* operand = instruction->operands[operand_index].get();
							if (!operand->is<insn>())
								continue;
							auto* definition = operand->as<insn>();
							if (!check(produces_value(definition->opc) && definition->vt != type::none, "operand refers to a valueless instruction", block.block,
									  instruction))
								return false;
							if (instruction->is<phi>()) {
								auto* predecessor = block.block->predecessors[operand_index];
								if (definition->parent == predecessor) {
									if (!check(instruction_positions.at(definition) < instruction_positions.at(predecessor->back()),
											  "phi operand is not defined before its predecessor terminator", block.block, instruction))
										return false;
								} else if (!check(dominates(definition->parent, predecessor), "phi operand does not dominate its predecessor terminator", block.block,
													instruction)) {
									return false;
								}
							} else if (definition->parent == block.block) {
								if (!check(instruction_positions.at(definition) < instruction_positions.at(instruction),
										  "instruction operand is not defined before its use", block.block, instruction))
									return false;
							} else if (!check(dominates(definition->parent, block.block), "instruction operand definition does not dominate its use", block.block,
												instruction)) {
								return false;
							}
						}
					}
				}
				return true;
			}

			value* ownership_root(value* operand) {
				if (auto found = ownership_roots.find(operand); found != ownership_roots.end())
					return found->second;

				std::vector<value*> path;
				value*              root = operand;
				while (root->is<insn>()) {
					if (auto found = ownership_roots.find(root); found != ownership_roots.end()) {
						root = found->second;
						break;
					}
					auto*  instruction = root->as<insn>();
					value* source      = nullptr;
					if (instruction->is<move>() || instruction->is<erase_type>() || instruction->is<assume_cast>())
						source = instruction->operands[0].get();
					else if (instruction->is<extract>() && instruction->operands[1]->as<constant>()->i32 == 0)
						source = instruction->operands[0].get();
					if (!source)
						break;
					path.push_back(root);
					root = source;
				}
				ownership_roots.emplace(root, root);
				for (auto* alias : path)
					ownership_roots[alias] = root;
				return root;
			}

			bool verify_ownership() {
				bool ownership_materialized = false;
				for (auto* instruction : all_instructions)
					ownership_materialized |= instruction->is<retain>() || instruction->is<release>();
				if (!ownership_materialized)
					return true;

				for (auto* instruction : all_instructions)
					if (is_reference_type(instruction->vt) && instruction->owner == ownership_kind::owned && instruction->use_count() == 0)
						return fail("owned result is neither consumed nor released", instruction->parent, instruction);

				for (auto& block : blocks) {
					std::unordered_set<value*> consumed;
					for (auto* instruction : block.instructions) {
						for (auto& operand_ref : instruction->operands) {
							auto* operand = operand_ref.get();
							if (!is_reference_type(operand->vt))
								continue;
							auto* root = ownership_root(operand);
							if (!check(!consumed.contains(root), "owned value is used after it was consumed on the same path", block.block, instruction))
								return false;
						}

						auto consume = [&](value* operand) {
							if (!is_reference_type(operand->vt))
								return true;
							auto* root = ownership_root(operand);
							if (!check(root->owner == ownership_kind::owned, "ownership consumer requires an owned value", block.block, instruction))
								return false;
							return check(consumed.emplace(root).second, "owned value is consumed more than once on the same path", block.block, instruction);
						};

						if (instruction->is<release>()) {
							if (!consume(instruction->operands[0].get()))
								return false;
						} else if (instruction->is<ret>()) {
							if (!consume(instruction->operands[0].get()))
								return false;
						} else if (instruction->is<deopt>()) {
							for (size_t index = 2; index < instruction->operands.size(); index += 2)
								if (!consume(instruction->operands[index].get()))
									return false;
						}
					}
				}
				return true;
			}
		};
	}

	std::string verification_error::describe() const {
		if (instruction && block)
			return util::fmt("IR verifier: %s [block $%x, instruction %u, opcode %u]", message.c_str(), unsigned(block->uid), unsigned(instruction->name),
				 unsigned(instruction->opc));
		if (instruction)
			return util::fmt("IR verifier: %s [instruction %u, opcode %u]", message.c_str(), unsigned(instruction->name), unsigned(instruction->opc));
		if (block)
			return util::fmt("IR verifier: %s [block $%x]", message.c_str(), unsigned(block->uid));
		return util::fmt("IR verifier: %s", message.c_str());
	}

	bool ir_verification_enabled() {
		static const bool enabled = LI_DEBUG || [] {
			const char* value = std::getenv("LI_IR_VERIFY");
			return value && std::strcmp(value, "1") == 0;
		}();
		return enabled;
	}

	std::optional<verification_error> verify_ir(procedure& proc) { return verifier{proc}.run(); }
}
