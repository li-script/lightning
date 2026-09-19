#include <ir/insn.hpp>
#include <ir/ownership.hpp>
#include <ir/proc.hpp>
#include <ir/runtime.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace li::ir {
	namespace {
		bool value_may_need_reference_counting(const value* v, std::unordered_map<const value*, uint8_t>& state) {
			if (v->vt != type::any) {
				if (!is_gc_data(v->vt))
					return false;
				if (v->is<constant>()) {
					auto* gc = v->as<constant>()->gc;
					return gc && !gc->is_static;
				}
				return true;
			}

			auto [it, inserted] = state.try_emplace(v, 1);
			if (!inserted)
				return it->second == 2;

			bool result = true;
			if (v->is<insn>()) {
				auto* instruction = v->as<insn>();
				if (instruction->is<assume_cast>() || instruction->is<move>() || instruction->is<erase_type>() || instruction->is<retain>()) {
					result = value_may_need_reference_counting(instruction->operands[0].get(), state);
				} else if (instruction->is<phi>()) {
					result = false;
					for (auto& operand : instruction->operands)
						result |= value_may_need_reference_counting(operand.get(), state);
				} else if (instruction->is<select>()) {
					result = value_may_need_reference_counting(instruction->operands[1].get(), state) ||
								value_may_need_reference_counting(instruction->operands[2].get(), state);
				} else if (instruction->is<compare>()) {
					// Comparisons produce either an immediate bool or the exception marker.
					result = false;
				} else if (instruction->is<ccall>() && instruction->operands[0]->as<constant>()->nfni == &runtime::binary_info) {
					auto op = static_cast<bc::opcode>(instruction->operands.back()->as<constant>()->i32);
					result  = op < bc::CEQ || op > bc::CLE;
				}
			}
			if (result)
				state[v] = 2;
			else
				state.erase(v);
			return result;
		}
	}

	bool value_may_need_reference_counting(const value* v) {
		std::unordered_map<const value*, uint8_t> state;
		return value_may_need_reference_counting(v, state);
	}

	bool is_ownership_transfer(const value* v) {
		if (!v->is<insn>())
			return false;
		auto* instruction = v->as<insn>();
		return instruction->is<move>() && instruction->owner == ownership_kind::borrowed && instruction->operands[0]->owner == ownership_kind::owned;
	}
}

namespace li::ir::opt {
	namespace {
		using value_set  = std::unordered_set<value*>;
		using block_sets = std::unordered_map<basic_block*, value_set>;

		size_t predecessor_index(basic_block* predecessor, basic_block* successor, size_t successor_slot);

		struct ownership_analysis {
			procedure*                            proc;
			std::unordered_map<value*, value*>    roots;
			std::unordered_map<value*, value_set> dependencies;
			block_sets                            uses;
			block_sets                            defs;
			block_sets                            live_in;
			block_sets                            live_out;

			explicit ownership_analysis(procedure* proc) : proc(proc) {}

			static bool is_reference(type vt) { return vt == type::any || is_gc_data(vt); }

			static value* transparent_source(insn* instruction) {
				if (instruction->is<assume_cast>() || instruction->is<move>() || instruction->is<erase_type>())
					return instruction->operands[0].get();
				if (instruction->is<extract>() && instruction->operands[1]->as<constant>()->i32 == 0)
					return instruction->operands[0].get();
				return nullptr;
			}

			void find_roots() {
				roots.clear();
				for (auto& block : proc->basic_blocks) {
					for (auto* instruction : block->insns()) {
						if (is_reference(instruction->vt) && !transparent_source(instruction) && instruction->owner == ownership_kind::owned)
							roots.emplace(instruction, instruction);
					}
				}

				bool changed;
				do {
					changed = false;
					for (auto& block : proc->basic_blocks) {
						for (auto* instruction : block->insns()) {
							if (!is_reference(instruction->vt) || roots.contains(instruction))
								continue;
							auto* source      = transparent_source(instruction);
							auto  source_root = source ? roots.find(source) : roots.end();
							if (source_root != roots.end()) {
								roots.emplace(instruction, source_root->second);
								changed = true;
							}
						}
					}
				} while (changed);
			}

			value* root(value* v) const {
				auto it = roots.find(v);
				return it == roots.end() ? nullptr : it->second;
			}

			const value_set& borrowed_dependencies(value* v) {
				auto [cached, inserted] = dependencies.try_emplace(v);
				if (!inserted)
					return cached->second;

				value_set result;
				if (auto* owned = root(v)) {
					result.emplace(owned);
				} else if (v->is<insn>()) {
					auto* instruction = v->as<insn>();
					for (auto& operand : instruction->operands) {
						if (!is_reference(operand->vt))
							continue;
						if (auto* operand_root = root(operand.get())) {
							result.emplace(operand_root);
						} else if (operand->is<insn>()) {
							auto& nested = borrowed_dependencies(operand.get());
							result.insert(nested.begin(), nested.end());
						}
					}
				}

				auto& stored = dependencies.find(v)->second;
				stored       = std::move(result);
				return stored;
			}

			void add_operand_uses(value_set& set, value* operand) {
				if (!is_reference(operand->vt))
					return;
				if (auto* owned = root(operand)) {
					set.emplace(owned);
				} else {
					auto& borrowed = borrowed_dependencies(operand);
					set.insert(borrowed.begin(), borrowed.end());
				}
			}

			void compute_liveness(bool include_phi_edges) {
				uses.clear();
				defs.clear();
				live_in.clear();
				live_out.clear();
				dependencies.clear();

				for (auto& owner : proc->basic_blocks) {
					auto* block      = owner.get();
					auto& block_uses = uses[block];
					auto& block_defs = defs[block];
					for (auto* instruction : block->insns()) {
						if (root(instruction) == instruction)
							block_defs.emplace(instruction);
						if (instruction->is<phi>())
							continue;
						value_set operands;
						for (auto& operand : instruction->operands)
							add_operand_uses(operands, operand.get());
						for (auto* used : operands)
							if (!block_defs.contains(used))
								block_uses.emplace(used);
					}
					live_in.emplace(block, value_set{});
					live_out.emplace(block, value_set{});
				}

				bool changed;
				do {
					changed = false;
					for (auto bit = proc->basic_blocks.rbegin(); bit != proc->basic_blocks.rend(); ++bit) {
						auto*     block = bit->get();
						value_set out;
						for (size_t successor_slot = 0; successor_slot != block->successors.size(); ++successor_slot) {
							auto* successor      = block->successors[successor_slot];
							auto& successor_live = live_in[successor];
							out.insert(successor_live.begin(), successor_live.end());
							if (include_phi_edges) {
								auto edge = predecessor_index(block, successor, successor_slot);
								for (auto* phi_value : successor->phis())
									add_operand_uses(out, phi_value->operands[edge].get());
							}
						}
						value_set in = uses[block];
						for (auto* value : out)
							if (!defs[block].contains(value))
								in.emplace(value);
						if (out != live_out[block] || in != live_in[block]) {
							live_out[block] = std::move(out);
							live_in[block]  = std::move(in);
							changed         = true;
						}
					}
				} while (changed);
			}

			value_set edge_live(basic_block* successor, size_t predecessor_index) {
				value_set result = live_in[successor];
				for (auto* phi_value : successor->phis())
					add_operand_uses(result, phi_value->operands[predecessor_index].get());
				return result;
			}
		};

		size_t predecessor_index(basic_block* predecessor, basic_block* successor, size_t successor_slot) {
			size_t occurrence = 0;
			for (size_t i = 0; i != successor_slot; ++i)
				occurrence += predecessor->successors[i] == successor;
			for (size_t i = 0; i != successor->predecessors.size(); ++i) {
				if (successor->predecessors[i] == predecessor && occurrence-- == 0)
					return i;
			}
			assume_unreachable();
		}

		bool is_lifetime_barrier(const insn* instruction) {
			constexpr uint32_t barriers = effect_write | effect_may_throw | effect_may_call_user | effect_release_may_destroy;
			return (instruction->effects & barriers) != 0;
		}

		basic_block* ensure_edge_block(procedure* proc, basic_block* successor, size_t predecessor_index, ownership_stats& stats) {
			auto* predecessor = successor->predecessors[predecessor_index];
			if (predecessor->successors.size() == 1)
				return predecessor;

			size_t occurrence = 0;
			for (size_t i = 0; i != predecessor_index; ++i)
				occurrence += successor->predecessors[i] == predecessor;
			size_t successor_slot = predecessor->successors.size();
			for (size_t i = 0; i != predecessor->successors.size(); ++i) {
				if (predecessor->successors[i] == successor && occurrence-- == 0) {
					successor_slot = i;
					break;
				}
			}
			LI_ASSERT(successor_slot != predecessor->successors.size());

			auto* edge       = proc->add_block();
			edge->bc_begin   = predecessor->bc_end;
			edge->bc_end     = predecessor->bc_end;
			edge->cold_hint  = predecessor->cold_hint;
			edge->loop_depth = predecessor->loop_depth;
			edge->predecessors.emplace_back(predecessor);
			edge->successors.emplace_back(successor);
			predecessor->successors[successor_slot]    = edge;
			successor->predecessors[predecessor_index] = edge;

			auto*  terminator                    = predecessor->back();
			size_t target_operand                = terminator->is<jmp>() ? 0 : successor_slot + 1;
			terminator->operands[target_operand] = proc->add_const(constant(edge));
			builder{edge}.emit<jmp>(successor);
			proc->mark_blocks_dirty();
			++stats.edges_split;
			return edge;
		}

		ref<insn> retain_after_definition(insn* definition) {
			builder b{definition};
			if (!definition->is<phi>())
				return b.emit_after<retain>(definition, definition);
			auto retained = b.create<retain>(definition->parent->proc, definition);
			auto position = definition->parent->end_phi();
			definition->parent->insert(position, retained);
			retained->update();
			return retained;
		}

		size_t count_rc_ops(procedure* proc) {
			size_t count = 0;
			for (auto& block : proc->basic_blocks)
				for (auto* instruction : block->insns())
					count += instruction->is<retain>() || instruction->is<release>();
			return count;
		}
	}

	ownership_stats ownership(procedure* proc) {
		ownership_stats stats;
		stats.rc_ops_before_elision = count_rc_ops(proc);
		if (stats.rc_ops_before_elision) {
			stats.rc_ops_after_elision = stats.rc_ops_before_elision;
			stats.already_processed    = true;
			return stats;
		}

		// A referenced PHI is an owning merge. Each dynamic incoming edge either
		// transfers an owned reference or creates one with retain.
		for (auto& block : proc->basic_blocks) {
			for (auto* instruction : block->phis()) {
				if (instruction->use_count() && ownership_analysis::is_reference(instruction->vt))
					instruction->owner = ownership_kind::owned;
			}
		}

		ownership_analysis analysis{proc};
		analysis.find_roots();

		// Negative frame slots (target, self, and arguments) are already owned by
		// the active VM frame. Loads from slots never written by this procedure may
		// stay borrowed across calls; the frame itself is their lifetime root.
		std::unordered_set<int32_t> mutated_frame_slots;
		for (auto& block : proc->basic_blocks) {
			for (auto* instruction : block->insns()) {
				if (instruction->is<store_local>()) {
					auto slot = instruction->operands[0]->as<constant>()->i32;
					if (slot < 0)
						mutated_frame_slots.emplace(slot);
				}
			}
		}
		auto borrows_immutable_frame_slot = [&](value* v) {
			while (v->is<insn>()) {
				auto* instruction = v->as<insn>();
				if (instruction->is<load_local>()) {
					auto slot = instruction->operands[0]->as<constant>()->i32;
					return slot < 0 && !mutated_frame_slots.contains(slot);
				}
				auto* source = ownership_analysis::transparent_source(instruction);
				if (!source)
					break;
				v = source;
			}
			return false;
		};

		// Borrowed results remain borrowed for local, effect-free use. Promote only
		// values that cross a block/effect boundary or escape through return.
		std::vector<insn*> promote;
		for (auto& block : proc->basic_blocks) {
			for (auto* definition : block->insns()) {
				if (!ownership_analysis::is_reference(definition->vt) || analysis.root(definition) || !definition->use_count() ||
					 borrows_immutable_frame_slot(definition))
					continue;
				bool escapes = false;
				definition->for_each_user([&](insn* user, size_t) {
					if (user->is<phi>())
						return false;
					if (user->is<ret>() || user->parent != definition->parent || is_lifetime_barrier(user)) {
						escapes = true;
						return true;
					}
					for (auto* between = definition->next; between != user; between = between->next) {
						if (is_lifetime_barrier(between)) {
							escapes = true;
							return true;
						}
					}
					return false;
				});
				if (escapes)
					promote.emplace_back(definition);
			}
		}

		for (auto* definition : promote) {
			std::vector<std::pair<insn*, size_t>> users;
			definition->for_each_user([&](insn* user, size_t operand) {
				users.emplace_back(user, operand);
				return false;
			});
			auto retained = retain_after_definition(definition);
			++stats.retains_inserted;
			for (auto [user, operand] : users) {
				user->operands[operand] = retained;
				user->update();
			}
		}

		analysis.find_roots();

		// Returning a borrowed source constant transfers a fresh caller reference.
		for (auto& block : proc->basic_blocks) {
			auto* terminator = block->back();
			if (!terminator->is<ret>() || !ownership_analysis::is_reference(terminator->operands[0]->vt) || analysis.root(terminator->operands[0].get()))
				continue;
			auto retained           = builder{terminator}.emit_before<retain>(terminator, terminator->operands[0]);
			terminator->operands[0] = retained;
			++stats.retains_inserted;
		}

		analysis.find_roots();
		analysis.compute_liveness(true);

		// Materialize ownership on PHI edges. Transfer one dead owned incoming per
		// edge/root; retain aliases needed after the merge or by another PHI.
		// Phi-edge liveness is included here so a value forwarded to a later PHI
		// is not also consumed by an earlier merge on the same dynamic path.
		std::vector<basic_block*> phi_blocks;
		phi_blocks.reserve(proc->basic_blocks.size());
		for (auto& block : proc->basic_blocks)
			phi_blocks.emplace_back(block.get());
		for (auto* successor : phi_blocks) {
			for (size_t edge_index = 0; edge_index != successor->predecessors.size(); ++edge_index) {
				value_set transferred;
				for (auto* phi_value : successor->phis()) {
					if (phi_value->owner != ownership_kind::owned)
						continue;
					auto& incoming      = phi_value->operands[edge_index];
					auto* incoming_root = analysis.root(incoming.get());
					bool  can_transfer  = incoming_root && !analysis.live_in[successor].contains(incoming_root) && transferred.emplace(incoming_root).second;
					if (can_transfer) {
						++stats.ownership_moves;
						++stats.retains_elided;
						++stats.releases_elided;
						continue;
					}

					auto* edge_block = ensure_edge_block(proc, successor, edge_index, stats);
					auto  retained   = builder{edge_block}.emit_before<retain>(edge_block->back(), incoming);
					incoming         = retained;
					++stats.retains_inserted;
				}
			}
		}

		analysis.find_roots();
		analysis.compute_liveness(true);

		struct release_after_request {
			insn*  after;
			value* owned;
		};
		struct edge_release_request {
			basic_block* successor;
			size_t       predecessor_index;
			value*       owned;
		};
		std::vector<release_after_request> release_after;
		std::vector<edge_release_request>  release_on_edge;

		for (auto& block_owner : proc->basic_blocks) {
			auto*                  block = block_owner.get();
			std::vector<value_set> edge_values;
			value_set              live;
			for (size_t successor_slot = 0; successor_slot != block->successors.size(); ++successor_slot) {
				auto* successor = block->successors[successor_slot];
				auto  edge      = predecessor_index(block, successor, successor_slot);
				auto  values    = analysis.edge_live(successor, edge);
				live.insert(values.begin(), values.end());
				edge_values.emplace_back(std::move(values));
			}

			for (auto* owned : live) {
				for (size_t successor_slot = 0; successor_slot != block->successors.size(); ++successor_slot) {
					if (edge_values[successor_slot].contains(owned))
						continue;
					auto* successor = block->successors[successor_slot];
					auto  edge      = predecessor_index(block, successor, successor_slot);
					release_on_edge.push_back({successor, edge, owned});
				}
			}

			for (auto it = block->rbegin(); it != block->rend(); ++it) {
				auto* instruction = *it;
				if (instruction->is<phi>())
					continue;
				value_set live_after = live;
				value_set instruction_uses;
				for (auto& operand : instruction->operands)
					analysis.add_operand_uses(instruction_uses, operand.get());
				value_set consumed;
				if (instruction->is<ret>() || instruction->is<deopt>())
					consumed = instruction_uses;
				for (auto* used : instruction_uses) {
					if (!live_after.contains(used) && !consumed.contains(used))
						release_after.push_back({instruction, used});
					live.emplace(used);
				}
				if (analysis.root(instruction) == instruction) {
					if (!live_after.contains(instruction))
						release_after.push_back({instruction, instruction});
					live.erase(instruction);
				}
			}
		}

		for (auto [after, owned] : release_after) {
			LI_ASSERT(!after->is_terminator());
			builder{after}.emit_after<release>(after, make_ref(owned));
			++stats.releases_inserted;
		}
		for (auto [successor, predecessor_index, owned] : release_on_edge) {
			auto* edge_block = ensure_edge_block(proc, successor, predecessor_index, stats);
			builder{edge_block}.emit_before<release>(edge_block->back(), make_ref(owned));
			++stats.releases_inserted;
		}

		// A call frame can adopt an owned operand when its final release is the
		// first operation after the call. The borrowed move is a lowering-only
		// marker: call_prepare publishes that slot without retaining it, and frame
		// truncation performs the original release at the same source boundary.
		analysis.find_roots();
		std::vector<insn*> calls;
		for (auto& block : proc->basic_blocks)
			for (auto* instruction : block->insns())
				if (instruction->is<vcall>())
					calls.emplace_back(instruction);

		for (auto* call : calls) {
			for (size_t operand_index = 0; operand_index != call->operands.size(); ++operand_index) {
				// FRAME_TARGET is immutable after vm_invoke finishes dispatch. Self and
				// argument slots are mutable inside the callee; transferring their last
				// caller reference could run a finalizer before the call returns.
				if (operand_index != 0)
					continue;
				auto* operand = call->operands[operand_index].get();
				if (operand->vt != type::fn)
					continue;
				auto* root = analysis.root(operand);
				if (!root)
					continue;

				auto* released = call->next;
				if (released == &call->parent->insn_list_head || !released->is<release>() || analysis.root(released->operands[0].get()) != root)
					continue;

				std::unordered_set<value*> visited;
				auto                       only_consumed_by_call = [&](auto&& self, value* current) -> bool {
					if (!visited.emplace(current).second)
						return true;
					bool valid = true;
					current->as<insn>()->for_each_user([&](insn* user, size_t index) {
						if ((user == call && index == operand_index) || user == released)
							return false;
						if (ownership_analysis::transparent_source(user) == current && self(self, user))
							return false;
						valid = false;
						return true;
					});
					return valid;
				};
				if (!only_consumed_by_call(only_consumed_by_call, root))
					continue;

				auto transferred              = builder{call}.emit_before<move>(call, call->operands[operand_index]);
				transferred->owner            = ownership_kind::borrowed;
				call->operands[operand_index] = transferred;
				call->update();
				call->parent->erase(instruction_iterator(released));
				++stats.ownership_moves;
				++stats.retains_elided;
				++stats.releases_elided;

				// A retained alias fed only to this call can reuse an ownership
				// reference whose release immediately precedes the transfer.
				if (!operand->is<retain>() || operand->use_count() != 1)
					continue;
				auto* source_root    = analysis.root(operand->as<retain>()->operands[0].get());
				auto* source_release = transferred->prev;
				if (!source_root || source_release == &call->parent->insn_list_head || !source_release->is<release>() ||
					 analysis.root(source_release->operands[0].get()) != source_root)
					continue;
				transferred->operands[0] = operand->as<retain>()->operands[0];
				call->parent->erase(instruction_iterator(source_release));
				operand->as<insn>()->parent->erase(instruction_iterator(operand->as<insn>()));
				++stats.pairs_elided;
				++stats.retains_elided;
				++stats.releases_elided;
			}
		}

		// Remove operations that are statically incapable of touching a GC
		// reference. This includes erased immediates and all-immediate PHIs.
		// Elided retains are forwarded in one procedure scan; a per-instruction
		// use replacement is quadratic in large scripts.
		std::unordered_map<value*, value*> forwarded;
		for (auto& block : proc->basic_blocks) {
			for (auto* instruction : block->insns()) {
				if (instruction->is<retain>() && !value_may_need_reference_counting(instruction->operands[0].get()))
					forwarded.emplace(instruction, instruction->operands[0].get());
			}
		}
		auto forward = [&](value* candidate) {
			while (true) {
				auto it = forwarded.find(candidate);
				if (it == forwarded.end())
					return candidate;
				candidate = it->second;
			}
		};
		// Every user is rewritten before any retain is erased: users may live in
		// earlier blocks, and erasing a still-referenced instruction frees it.
		// Chained retains are rewritten too so no elided retain keeps another alive.
		if (!forwarded.empty()) {
			for (auto& block : proc->basic_blocks) {
				for (auto* instruction : block->insns()) {
					bool rewritten = false;
					for (auto& operand : instruction->operands) {
						if (!forwarded.contains(operand.get()))
							continue;
						operand   = make_use(forward(operand.get()));
						rewritten = true;
					}
					if (rewritten)
						instruction->update();
				}
			}
		}
		for (auto& block : proc->basic_blocks) {
			for (auto it = block->begin(); it != block->end();) {
				auto* instruction = it.at;
				++it;
				if (forwarded.contains(instruction)) {
					LI_ASSERT(instruction->use_count() == 0);
					instruction->operands.clear();
					block->erase(instruction_iterator(instruction));
					++stats.retains_elided;
				} else if (instruction->is<release>() && !value_may_need_reference_counting(instruction->operands[0].get())) {
					block->erase(instruction_iterator(instruction));
					++stats.releases_elided;
				}
			}
		}

		stats.rc_ops_before_elision = count_rc_ops(proc) + stats.retains_elided + stats.releases_elided;

		// The only timing-insensitive local cancellation is a fresh retain whose
		// sole consumer is the immediately following release.
		for (auto& block : proc->basic_blocks) {
			for (auto it = block->begin(); it != block->end();) {
				auto* retained = it.at;
				++it;
				if (!retained->is<retain>() || retained->use_count() != 1 || retained->next == &block->insn_list_head)
					continue;
				auto* released = retained->next;
				if (!released->is<release>() || released->operands[0].get() != retained)
					continue;
				auto next = instruction_iterator(released->next);
				block->erase(instruction_iterator(released));
				block->erase(instruction_iterator(retained));
				it = next;
				++stats.pairs_elided;
				++stats.retains_elided;
				++stats.releases_elided;
			}
		}

		stats.rc_ops_after_elision = count_rc_ops(proc);
		return stats;
	}
}
