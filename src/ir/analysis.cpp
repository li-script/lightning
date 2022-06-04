#include <ir/proc.hpp>

namespace li::ir {
	// TODO: Cache with a dominance tree, switch to less expensive algorithm.
	//
	bool basic_block::dom(const basic_block* n) const {
		auto mark  = ++n->proc->next_visited_mark;
		auto nodom = [this, mark](auto& self, const basic_block* n) -> bool {
			if (n->predecessors.empty())
				return true;
			n->visited = mark;
			for (auto& s : n->predecessors) {
				if (s->visited != mark && s != this) {
					if (self(self, s))
						return true;
				}
			}
			return false;
		};
		return this == n || !nodom(nodom, n);
	}
	bool basic_block::postdom(const basic_block* n) const {
		auto mark  = ++n->proc->next_visited_mark;
		auto nodom = [this, mark](auto& self, const basic_block* n) -> bool {
			if (n->successors.empty())
				return true;
			n->visited = mark;
			for (auto& s : n->successors) {
				if (s->visited != mark && s != this) {
					if (self(self, s))
						return true;
				}
			}
			return false;
		};
		return this == n || !nodom(nodom, n);
	}

	// Returns true if this block can reach the block.
	//
	bool basic_block::check_path(const basic_block* to) const {
		return proc->bfs([&](const basic_block* b) { return b == to; }, this);
	}
};