#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>

namespace li::ir::opt {

	// TODO: Global.
	//
	static bool is_identical(const value_ref& a, const value_ref& b) {
		if (a == b) {
			return true;
		}
		if (a->is<insn>() && b->is<insn>()) {
			//auto* ai = a->get<insn>();
			//auto* bi = b->get<insn>();
			//if (ai->op == bi->op && ai->is_pure) {
			//
			//}
			// TODO: Trace until common dominator to ensure no side effects, compare by value.
		}
		return false;
	}

	// Applies identical value folding.
	//
	void fold_identical(procedure* proc, bool local) {
		for (auto& bb : proc->basic_blocks) {
			for (auto it = bb->instructions.rbegin(); it != bb->instructions.rend(); ++it) {
				auto& ins = *it;
				if (ins->is_pure && !ins->sideffect && !ins->is_volatile) {
					std::shared_ptr<insn> found;
					for (auto it2 = std::next(it); it2 != bb->instructions.rend(); ++it2) {
						auto& ins2 = *it2;
						if (ins2->op == ins->op && ins2->operands.size() == ins->operands.size()) {
							if (std::equal(ins->operands.begin(), ins->operands.end(), ins2->operands.begin(), is_identical)) {
								found = ins2;
								break;
							}
						}
						if (ins2->sideffect && !ins->is_pure) {
							break;
						}
					}
					if (found)
						bb->proc->replace_all_uses(ins.get(), found);
				}
			}
		}
	}
};