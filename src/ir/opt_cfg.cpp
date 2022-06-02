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
			if (bb->instructions.size() != 1) {
				auto& jcc = bb->instructions.back();
				if (jcc->op == opcode::jcc) {
					value_ref opt = nullptr;

					if (jcc->operands[1] == jcc->operands[2]) {
						opt = jcc->operands[1];
					} else if (jcc->operands[0]->is<constant>()) {
						auto cc = jcc->operands[0]->get<constant>();
						opt = jcc->operands[cc->i1 ? 1 : 2];

						auto& opt_out = jcc->operands[cc->i1 ? 2 : 1];
						proc->del_jump(bb.get(), opt_out->get<constant>()->bb);
					}

					if (opt) {
						builder b{bb.get()};
						auto new_ins = b.emit<jmp>(opt);
						jcc->copy_debug_info(new_ins.get());
						jcc = new_ins;
					}
				}

				++it;
				continue;
			}

			// Delete useless blocks.
			//
			auto& term = bb->instructions.back();
			if (term->op == opcode::jmp) {
				for (auto& pred : bb->predecesors) {
					auto& t2 = pred->instructions.back();
					for (auto& op : t2->operands) {
						if (op->is<constant>() && op->get<constant>()->bb == bb.get()) {
							op = term->operands[0];
						}
					}
					proc->del_jump(pred, bb.get());
					proc->del_jump(bb.get(), term->operands[0]->get<constant>()->bb);
					proc->add_jump(pred, term->operands[0]->get<constant>()->bb);
				}
			}
			it = proc->del_block(bb.get());
		}
	}
};