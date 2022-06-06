#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>

namespace li::ir::opt {
	// Prepares the IR to be lifted to MIR.
	//
	void prepare_for_mir(procedure* proc) {
	}
	void finalize_for_mir(procedure* proc) {
		// Fixup PHIs, this should be the last operation as they are not allowed to be optimized out.
		//
		for (auto& bb : proc->basic_blocks) {
			for (auto phi : bb->insns()) {
				if (!phi->is<ir::phi>())
					break;
				for (size_t i = 0; i != phi->operands.size(); i++) {
					if (phi->vt == type::unk && phi->operands[i]->vt != type::unk)
						phi->operands[i] = builder{}.emit_before<erase_type>(bb->predecessors[i]->back(), phi->operands[i]);
					else
						phi->operands[i] = builder{}.emit_before<move>(bb->predecessors[i]->back(), phi->operands[i]);
				}
			}
		}

		// Topologically sort the procedure and rename every register.
		//
		proc->topological_sort();

		// Add loop hints.
		//  TODO: Not reaaaly + move to procedure.
		//
		for (auto& blk : proc->basic_blocks) {
			for (auto& s : blk->successors) {
				if (s->uid < blk->uid) {
					for (size_t i = s->uid; i <= blk->uid; ++i) {
						proc->basic_blocks[i]->loop_depth++;
					}
				}
			}
		}

		// Reset the names and validate.
		//
		proc->reset_names();
		proc->validate();
		proc->print();
	}
};