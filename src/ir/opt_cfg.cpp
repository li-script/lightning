#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>

namespace li::ir::opt {
	// Optimizes the control flow graph.
	//
	void cfg(procedure* proc) {
		bool changed;
		do {
			changed = proc->remove_unreachable_blocks() != 0;

			for (auto it = proc->basic_blocks.begin(); it != proc->basic_blocks.end();) {
				auto& bb = *it;

				// TODO: Optimize if unreachable terminated & not debug mode.
				//

				// Delete if there is no predecessors.
				//
				if (bb->predecessors.empty() && it != proc->basic_blocks.begin()) {
					while (!bb->successors.empty())
						proc->del_jump(bb.get(), bb->successors.back());
					it      = proc->del_block(bb.get());
					changed = true;
					continue;
				}

				// Optimize JCCs.
				//
				auto term = bb->back();
				if (term->is<jcc>()) {
					ref<> opt = nullptr;
					// Swap destination if chained to LNOT.
					//
					if (term->operands[0]->is<unop>() && term->operands[1] != term->operands[2]) {
						auto* ins = term->operands[0]->as<unop>();
						LI_ASSERT(ins->operands[0]->as<constant>()->vmopr == bc::LNOT);
						term->operands[0] = ins->operands[1];
						std::swap(term->operands[1], term->operands[2]);
						std::swap(bb->successors[0], bb->successors[1]);
						proc->mark_blocks_dirty();
					}

					// Same destination.
					//
					if (term->operands[1] == term->operands[2]) {
						opt          = term->operands[1];
						auto* target = opt->as<constant>()->bb;

						// Parallel edges into the same block can carry distinct
						// PHI values. Materialize that edge choice before
						// collapsing the JCC to a single jump.
						//
						size_t incoming[2] = {};
						size_t count       = 0;
						for (size_t i = 0; i != target->predecessors.size(); ++i) {
							if (target->predecessors[i] == bb.get()) {
								LI_ASSERT(count < 2);
								incoming[count++] = i;
							}
						}
						LI_ASSERT(count == 2);
						for (auto* phi : target->phis()) {
							auto  true_value  = phi->operands[incoming[0]];
							auto  false_value = phi->operands[incoming[1]];
							ref<> selected;
							if (true_value == false_value) {
								selected = true_value;
							} else if (term->operands[0]->is<constant>()) {
								selected = term->operands[0]->as<constant>()->i1 ? true_value : false_value;
							} else {
								selected = builder{term}.emit_before<select>(term, term->operands[0], true_value, false_value);
							}
							phi->operands[incoming[1]] = selected;
						}
						proc->del_jump(bb.get(), target);
						for (auto* phi : target->phis())
							phi->update();
					}
					// Constant test.
					//
					else if (term->operands[0]->is<constant>()) {
						auto cc       = term->operands[0]->as<constant>();
						opt           = term->operands[cc->i1 ? 1 : 2];
						auto& opt_out = term->operands[cc->i1 ? 2 : 1];
						proc->del_jump(bb.get(), opt_out->as<constant>()->bb);
					}

					if (opt) {
						auto new_term = builder{}.emit_after<jmp>(term, opt);
						term->erase();
						term    = new_term;
						changed = true;
					}
				}

				// Set cold hint if unreachable.
				//
				if (term->is<unreachable>()) {
					bb->cold_hint = 100;
				}

				// Delete blocks with only jmp. Do not create a second edge from
				// a predecessor to the target: predecessor occurrence order is
				// the PHI edge identity, and splicing at the jump block's target
				// position could reverse those parallel edges.
				//
				bool bypass_would_duplicate_edge = false;
				if (term->is<jmp>()) {
					auto* target = bb->successors.front();
					for (auto* predecessor : bb->predecessors) {
						if (range::find(predecessor->successors, target) != predecessor->successors.end()) {
							bypass_would_duplicate_edge = true;
							break;
						}
					}
				}
				if (bb->front() == bb->back() && term->is<jmp>() && bb.get() != proc->get_entry() && bb->successors.front() != bb.get() &&
					 !bb->predecessors.empty() && !bypass_would_duplicate_edge) {
					auto* target = bb->successors.front();

					// Duplicate PHI operands N-1 times.
					//
					auto predecessor = range::find(target->predecessors, bb.get());
					LI_ASSERT(predecessor != target->predecessors.end());
					auto pit = predecessor - target->predecessors.begin();
					for (auto* phi : target->phis()) {
						LI_ASSERT(phi->operands.size() == target->predecessors.size());
						auto incoming = phi->operands[pit];
						phi->operands.insert(phi->operands.begin() + pit, bb->predecessors.size() - 1, incoming);
					}

					// Fixup jump lists.
					//
					target->predecessors.insert(target->predecessors.begin() + pit, bb->predecessors.size() - 1, nullptr);
					range::copy(bb->predecessors, target->predecessors.begin() + pit);
					for (auto& pred : bb->predecessors) {
						*range::find(pred->successors, bb.get()) = target;
						for (auto& op : pred->back()->operands) {
							if (op->is<constant>() && op->as<constant>()->bb == bb.get()) {
								op = term->operands[0];
							}
						}
					}
					bb->predecessors.clear();
					bb->successors.clear();
					proc->mark_blocks_dirty();
					it      = proc->del_block(bb.get());
					changed = true;
					continue;
				}

				// Join jumps to blocks with a single predecessor.
				//
				if (term->is<jmp>() && bb->successors.front() != bb.get() && bb->successors.front() != proc->get_entry() &&
					 bb->successors.front()->predecessors.size() == 1) {
					auto* target = bb->successors.front();

					// Move the instruction list.
					//
					term->erase();
					while (!target->empty()) {
						auto i = target->front();
						if (i->is<phi>()) {
							i->replace_all_uses(i->operands[0]);
							i->erase();
						} else {
							bb->push_back(i->erase());
						}
					}

					// Fixup jump lists.
					//
					bb->successors.swap(target->successors);
					for (auto& suc : bb->successors) {
						*range::find(suc->predecessors, target) = bb.get();
					}

					target->predecessors.clear();
					target->successors.clear();
					proc->mark_blocks_dirty();
					it      = proc->del_block(target);
					changed = true;
					continue;
				}

				/*
				-- Block $0
				#1     %2:i1 = test_type %1:?, number
				#3     jcc %2:i1, $2, $1
				-- Block $1
				#5     set_exception str: expected variable 'depth' to be of type 'number'
				#7     ret exc
				-- Block $2
				#9     jcc %2:i1, $4, $3
				*/
				/*
				-- Block $x
				#1e    %14:? = phi $1->%6:nil, $3->%11:?
				#1d    jmp $7
				*/
				// ^ todo:
				++it;
			}
		} while (changed);

		proc->mark_blocks_dirty();
		proc->validate();
	}
};