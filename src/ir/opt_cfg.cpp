#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>

namespace li::ir::opt {
	// Optimizes the control flow graph.
	//
	void cfg(procedure* proc) {
		for (auto it = proc->basic_blocks.begin(); it != proc->basic_blocks.end();) {
			auto& bb = *it;

			// Optimize JCCs.
			//
			auto term = bb->back();
			if (term->is<jcc>()) {
				ref<> opt = nullptr;

				if (term->operands[1] == term->operands[2]) {
					opt = term->operands[1];
				} else if (term->operands[0]->is<constant>()) {
					auto cc = term->operands[0]->as<constant>();
					opt     = term->operands[cc->i1 ? 1 : 2];

					auto& opt_out = term->operands[cc->i1 ? 2 : 1];
					proc->del_jump(bb.get(), opt_out->as<constant>()->bb);
				}

				if (opt) {
					auto new_term = builder{}.emit_after<jmp>(term, opt);
					term->erase();
					term = new_term;
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
				it = proc->del_block(bb.get());
				continue;
			}

			/*
			-- Block $4
			#1e    %14:? = phi $1->%6:nil, $3->%11:?
			#1d    jmp $7
			*/
			// ^ todo:
			++it;
		}

		proc->mark_blocks_dirty();
		proc->validate();
	}
};