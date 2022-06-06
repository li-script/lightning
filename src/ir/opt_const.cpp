#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>

namespace li::ir::opt {
	// Folds constants.
	//
	void fold_constant(procedure* proc) {
		// TODO: Binop/Unop.

		for (auto& bb : proc->basic_blocks) {
			bb->erase_if([&](insn* ins) {
				if (ins->is<select>() && ins->operands[0]->is<constant>()) {
					ins->replace_all_uses(ins->operands[0]->as<constant>()->i1 ? ins->operands[1] : ins->operands[2]);
					return true;
				}
				return false;
			});
		}
	}
};