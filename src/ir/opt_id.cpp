#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>

namespace li::ir::opt {
	struct identity_check_record {
		identity_check_record* prev = nullptr;
		const value*      a;
		const value*      b;
	};
	static bool is_identical(const value* a, const value* b, identity_check_record* prev = nullptr) {
		// If same value, assume equal.
		//
		if (a == b)
			return true;

		// If in the equality check stack already, assume equal. (?)
		//
		for (auto it = prev; it; it = it->prev)
			if (it->a == a && it->b == b)
				return true;

		// If both are instructions:
		//
		if (a->is<insn>() && b->is<insn>()) {
			// If there are side effects or instruction is not pure, assume false.
			//
			auto ai = a->as<insn>();
			auto bi = b->as<insn>();
			if (ai->sideffect || !ai->is_pure)
				return false;

			// If opcode is the same and operand size matches:
			//
			if (ai->opc == bi->opc && ai->operands.size() == bi->operands.size()) {
				// Make a new record to fix infinite-loops with PHIs.
				//
				identity_check_record rec{prev, a, b};

				// If all values are equal, pass the check.
				//
				for (size_t i = 0; i != ai->operands.size(); i++)
					if (!is_identical(ai->operands[i], bi->operands[i], &rec))
						return false;
				return true;
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