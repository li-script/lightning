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
			if (bb->front() != bb->back()) {
				auto jc = bb->back();
				if (jc->is<jcc>()) {
					ref<> opt = nullptr;

					if (jc->operands[1] == jc->operands[2]) {
						opt = jc->operands[1];
					} else if (jc->operands[0]->is<constant>()) {
						auto cc = jc->operands[0]->as<constant>();
						opt     = jc->operands[cc->i1 ? 1 : 2];

						auto& opt_out = jc->operands[cc->i1 ? 2 : 1];
						proc->del_jump(bb.get(), opt_out->as<constant>()->bb);
					}

					if (opt) {
						builder{}.emit_after<jmp>(jc, opt);
						bb->erase(jc.get());
					}
				}

				++it;
				continue;
			}

			// Delete useless blocks.
			//
			auto term = bb->back();
			if (term->is<jmp>()) {
				for (auto& pred : bb->predecesors) {
					auto t2 = pred->back();
					for (auto& op : t2->operands) {
						if (op->is<constant>() && op->as<constant>()->bb == bb.get()) {
							op = term->operands[0];
						}
					}
					proc->del_jump(pred, bb.get());
					proc->del_jump(bb.get(), term->operands[0]->as<constant>()->bb);
					proc->add_jump(pred, term->operands[0]->as<constant>()->bb);
				}
			}
			it = proc->del_block(bb.get());

			// ^ todo:
			// z:
			//  jmp a
			// a: <-- only successor is z
			//  <stuff> 
			//  jmp b
		}
		proc->validate();
	}
};