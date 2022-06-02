#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>

namespace li::ir::opt {

	// TODO: Global.
	//
	static bool is_identical(const value* a, const value* b) {
		if (a == b) {
			return true;
		}
		if (a->is<insn>() && b->is<insn>()) {
			//auto* ai = a->as<insn>();
			//auto* bi = b->as<insn>();
			//if (ai->opc == bi->opc && ai->is_pure) {
			//
			//}
			// TODO: Trace until common dominator to ensure no side effects, compare by value.
			// ^ once this is done, can also be used for PHI nodes.
		}
		return false;
	}

	// Applies identical value folding.
	//
	void fold_identical(procedure* proc, bool local) {
		for (auto& bb : proc->basic_blocks) {
			for (insn* ins : view::reverse(bb->insns())) {
				if (ins->is_pure && !ins->sideffect && !ins->is_volatile) {
					insn* found = nullptr;

					for (insn* ins2 : bb->before(ins)) {
						if (ins2->opc == ins->opc && ins2->operands.size() == ins->operands.size()) {
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
						ins->replace_all_uses(found);
				}
			}
		}
		proc->validate();
	}
};