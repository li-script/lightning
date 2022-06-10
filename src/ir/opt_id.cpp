#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>

namespace li::ir::opt {

	static bool is_identical(const value* a, const value* b) {
		if (a == b) {
			return true;
		}
		if (a->is<insn>() && b->is<insn>()) {
			auto ai = a->as<insn>();
			auto bi = b->as<insn>();
			if (ai->sideffect || !ai->is_pure)
				return false;
			if (ai->opc == bi->opc && ai->operands.size() == bi->operands.size()) {
				if (std::equal(ai->operands.begin(), ai->operands.end(), bi->operands.begin(), is_identical)) {
					return true;
				}
			}
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
				if (!ins->is_volatile) {
					insn* found = nullptr;
					bool  fail  = false;
					for (insn* ins2 : bb->before(ins)) {
						if (is_identical(ins, ins2)) {
							found = ins2;
							break;
						}
						if (ins2->sideffect && !ins->is_const) {
							fail = true;
							break;
						}
					}

					// TODO: Global.
					//
					if (!fail && !found && bb->predecessors.size() == 1) {
						for (insn* ins2 : bb->predecessors.back()->insns()) {
							if (is_identical(ins, ins2)) {
								found = ins2;
								break;
							}
							if (ins2->sideffect && !ins->is_const) {
								break;
							}
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