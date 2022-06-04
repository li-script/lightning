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

	// Splits the basic block at instruction boundary and adds a jump to the next block after it.
	//
	basic_block* basic_block::split_at(const insn* at) {
		LI_ASSERT(at->parent == this);
		auto* blk = proc->add_block();

		// Split and move the instruction stream
		//
		if (at->next != end()) {
			auto nb                  = at->next;
			auto ne                  = std::prev(end()).at;
			blk->insn_list_head.next = nb;
			blk->insn_list_head.prev = ne;
			ne->next                 = &blk->insn_list_head;
			nb->prev                 = &blk->insn_list_head;
			for (auto it = nb; it != ne; ++it) {
				LI_ASSERT(it->parent == this);
				it->parent = blk;
			}
		}

		// Append the jump.
		//
		builder{}.emit_after<jmp>((insn*) at, blk);

		// Fixup the successor / predecessor list.
		//
		blk->successors  = std::move(successors);
		blk->predecessors = {blk};
		successors       = {blk};
		for (auto& suc : blk->successors) {
			for (auto& pred : suc->predecessors) {
				if (pred == this) {
					pred = blk;
					break;
				}
			}
		}

		// Dirty the toplogical sorting.
		//
		proc->is_topologically_sorted = false;
		return blk;
	}
};