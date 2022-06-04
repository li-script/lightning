#include <ir/insn.hpp>
#include <ir/proc.hpp>
#include <util/enuminfo.hpp>

namespace li::ir {
	// Replace implementation.
	//
	size_t insn::replace_all_uses(value* with) const {
		LI_ASSERT(parent);
		size_t n = 0;
		for (auto& b : parent->proc->basic_blocks) {
			n += replace_all_uses_in_block(with, b.get());
		}
		return n;
	}
	size_t insn::replace_all_uses_in_block(value* with, basic_block* bb) const {
		LI_ASSERT(parent);
		size_t n = 0;
		for (auto i : *(bb ? bb : parent)) {
			if (i == with)
				continue;
			for (auto& op : i->operands) {
				if (op.get() == this) {
					op.reset(with);
					n++;
				}
			}
		}
		return n;
	}
	size_t insn::replace_all_uses_outside_block(value* with) const {
		LI_ASSERT(parent);
		size_t n = 0;
		for (auto& b : parent->proc->basic_blocks) {
			if (b.get() != parent)
				n += replace_all_uses_in_block(with, b.get());
		}
		return n;
	}

	// User enumeration.
	//
	bool insn::for_each_user(util::function_view<bool(insn*, size_t)> cb) const {
		LI_ASSERT(parent);
		for (auto& b : parent->proc->basic_blocks) {
			if (for_each_user_in_block(cb, b.get()))
				return true;
		}
		return false;
	}
	bool insn::for_each_user_in_block(util::function_view<bool(insn*, size_t)> cb, basic_block* bb) const {
		LI_ASSERT(parent);
		for (auto i : *(bb ? bb : parent)) {
			for (size_t j = 0; j != i->operands.size(); j++) {
				if (i->operands[j].get() == this) {
					if (cb(i, j))
						return true;
				}
			}
		}
		return false;
	}
	bool insn::for_each_user_outside_block(util::function_view<bool(insn*, size_t)> cb) const {
		LI_ASSERT(parent);
		for (auto& b : parent->proc->basic_blocks) {
			if (b.get() != parent)
				if (for_each_user_in_block(cb, b.get()))
					return true;
		}
		return false;
	}

	// Splits the basic block at instruction boundary returns the new block. User must add the new terminator.
	//
	basic_block* basic_block::split_at(const insn* at) {
		LI_ASSERT(at->parent == this);
		auto* blk = proc->add_block();

		// Split and move the instruction stream
		//
		LI_ASSERT(at->next != end());
		auto it = back();
		while (true) {
			if (it == at)
				break;
			LI_ASSERT(it->parent == this);
			auto* p = it->prev;
			blk->push_front(it->erase());
			it = p;
		}

		// Fixup the successor / predecessor list.
		//
		for (auto& suc : successors) {
			for (auto& pred : suc->predecessors) {
				if (pred == this) {
					pred = blk;
					break;
				}
			}
		}
		successors.swap(blk->successors);

		// Dirty the analysis.
		//
		proc->mark_blocks_dirty();
		return blk;
	}
};