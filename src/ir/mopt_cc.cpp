#include <ir/opt.hpp>

namespace li::ir::opt {
	// Sink a single-use comparison next to its boolean consumer. Comparisons keep
	// producing ordinary GP values; this pass never introduces target flags or
	// changes floating-point condition semantics.
	//
	void remove_redundant_setcc(mprocedure* proc) {
		for (auto& bb : proc->basic_blocks) {
			if (bb.instructions.size() < 2)
				continue;

			size_t term_index = bb.instructions.size() - 1;
			minsn& term       = bb.instructions[term_index];
			if (!term.is(vop::js) || !term.arg[0].is_reg())
				continue;
			mreg condition = term.arg[0].reg;

			size_t def_index = term_index;
			for (size_t i = term_index; i-- != 0;) {
				if (bb.instructions[i].out == condition) {
					def_index = i;
					break;
				}
			}
			if (def_index == term_index || !bb.instructions[def_index].is_compare() || def_index + 1 == term_index)
				continue;

			// The result must be consumed only by this branch. Otherwise sinking the
			// definition would move it past another use.
			//
			size_t reads = 0;
			for (auto& block : proc->basic_blocks) {
				for (auto& ins : block.instructions)
					ins.for_each_reg([&](mreg r, bool read) { reads += read && r == condition; });
			}
			if (reads != 1)
				continue;

			minsn&              compare    = bb.instructions[def_index];
			std::array<mreg, 4> inputs     = {};
			size_t              num_inputs = 0;
			compare.for_each_reg([&](mreg r, bool read) {
				if (read && num_inputs != inputs.size())
					inputs[num_inputs++] = r;
			});

			bool blocked = false;
			for (size_t i = def_index + 1; i != term_index && !blocked; i++) {
				minsn& crossed = bb.instructions[i];
				if (crossed.has_side_effects()) {
					blocked = true;
					break;
				}
				crossed.for_each_reg_w_implicit([&](mreg r, bool read) {
					if (read)
						return;
					for (size_t n = 0; n != num_inputs; n++)
						blocked |= r == inputs[n];
				});
			}
			if (blocked)
				continue;

			std::rotate(bb.instructions.begin() + def_index, bb.instructions.begin() + def_index + 1, bb.instructions.begin() + term_index);
		}
	}
}
