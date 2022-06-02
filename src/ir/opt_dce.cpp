#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>

namespace li::ir::opt {
	// TODO: Global.
	//

	// Applies dead code elimination.
	//
	void dce(procedure* proc, bool local) {
		proc->toplogical_sort();
		for (auto& bb : proc->basic_blocks) {
			bb->erase_if([&](insn* ins) { return !ins->use_count() && !ins->is_volatile && !ins->sideffect && !ins->is_terminator(); });
		}
		proc->validate();
	}
};