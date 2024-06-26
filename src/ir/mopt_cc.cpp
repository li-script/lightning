#include <ir/opt.hpp>

namespace li::ir::opt {
	// Attempts to optimize-out any SETCC's by moving them nearby to the JS.
	//
	void remove_redundant_setcc(mprocedure* proc) {
		// TODO: Also for vop::select.

		// Try to move any compares closer to JS to avoid register allocation.
		//
		for (auto& bb : proc->basic_blocks) {
			// Skip if it does not end with a JS.
			//
			if (bb.instructions.empty())
				continue;
			auto& term = bb.instructions.back();
			if (!term.is(vop::js))
				continue;

			// Skip if branch condition is not defined in this block.
			//
			auto rview = view::reverse(bb.instructions);
			auto setcc = range::find_if(rview, [&](minsn& i) { return i.out == term.arg[0].reg; });
			if (setcc == rview.end())
				continue;
			if (!setcc->is(vop::setcc))
				continue;
			auto comperator = range::find_if(range::subrange(setcc, rview.end()), [&](minsn& i) { return i.out == setcc->arg[0].reg; });
			if (comperator == rview.end())
				continue;

			// Skip if value will be killed if we move it.
			//
			auto kill = range::find_if(range::subrange(comperator.base() - 1, bb.instructions.end() - 1), [&](minsn& i) {
				if (&i == &*setcc)
					return false;
				if (i.has_side_effects())
					return true;
				for (auto& livereg : comperator->arg)
					if (livereg.is_reg() && livereg.reg == i.out)
						return true;
				return false;
			});
			if (kill != (bb.instructions.end() - 1))
				continue;

			// Skip if value is alive after the block.
			// - TODO: Not really, have to implement recursive descent with write checks.
			//
			bool used_outside = false;
			for (auto& blk : proc->basic_blocks) {
				if (used_outside)
					break;
				if (&blk != &bb) {
					for (auto& ins : blk.instructions) {
						if (ins.reads_from_register(setcc->out)) {
							used_outside = true;
							break;
						}		
					}
				}
			}
			if (used_outside)
				continue;

			// Re-insert after moving the instruction.
			//
			auto cmp    = *comperator;
			*comperator = {vop::null, {}};
			*setcc      = {vop::null, {}};
			bb.instructions.insert(bb.instructions.end() - 1, cmp);
			bb.instructions.back().arg[0] = cmp.out;
			std::erase_if(bb.instructions, [](minsn& i) { return i.is_null(); });
		}
	}
};