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
			changed = false;

			for (auto it = proc->basic_blocks.begin(); it != proc->basic_blocks.end();) {
				auto& bb = *it;

				// TODO: Optimize if unreachable terminated & not debug mode.
				//

				// Delete if there is no predecessors.
				//
				if (bb->predecessors.empty() && it != proc->basic_blocks.begin()) {
					for (auto& suc : bb->successors) {
						proc->del_jump(bb.get(), suc);
					}
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
					if (term->operands[0]->is<unop>()) {
						auto* ins = term->operands[0]->as<unop>();
						LI_ASSERT(ins->operands[0]->as<constant>()->vmopr == bc::LNOT);
						term->operands[0] = ins->operands[1];
						std::swap(term->operands[1], term->operands[2]);
					}

					// Same destination.
					//
					if (term->operands[1] == term->operands[2]) {
						opt = term->operands[1];
					}
					// Constant test.
					//
					else if (term->operands[0]->is<constant>()) {
						auto cc = term->operands[0]->as<constant>();
						opt     = term->operands[cc->i1 ? 1 : 2];

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

				// Delete blocks with only jmp.
				//
				if (bb->front() == bb->back() && term->is<jmp>()) {
					auto* target = bb->successors.front();

					// Duplicate PHI operands N-1 times.
					//
					auto pit = range::find(target->predecessors, bb.get()) - target->predecessors.begin();
					for (auto* phi : target->phis()) {
						phi->operands.insert(phi->operands.begin() + pit, bb->predecessors.size() - 1, phi->operands[pit]);
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
					it      = proc->del_block(bb.get());
					changed = true;
					continue;
				}

				// Join jumps to blocks with a single predecessor.
				//
				if (term->is<jmp>() && bb->successors.front()->predecessors.size() == 1) {
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
					proc->del_block(target);
					changed = true;
				}

				/*
				-- Block $4
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