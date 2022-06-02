#include <ir/proc.hpp>

namespace li::ir {
	// TODO: Cache with a dominance tree, switch to less expensive algorithm.
	//
	bool basic_block::dom(basic_block* n) {
		for (auto& b : this->proc->basic_blocks) {
			b->visited = false;
		}
		auto nodom = [this](auto& self, basic_block* n) -> bool {
			if (n->predecesors.empty())
				return true;
			n->visited = true;
			for (auto& s : n->predecesors) {
				if (!s->visited && s != this) {
					if (self(self, s))
						return true;
				}
			}
			return false;
		};
		return this == n || !nodom(nodom, n);
	}
	bool basic_block::postdom( basic_block* n ) {
		for (auto& b : this->proc->basic_blocks) {
			b->visited = false;
		}
		auto nodom = [this](auto& self, basic_block* n) -> bool {
			if (n->successors.empty())
				return true;
			n->visited = true;
			for (auto& s : n->successors) {
				if (!s->visited && s != this) {
					if (self(self, s))
						return true;
				}
			}
			return false;
		};
		return this == n || !nodom(nodom, n);
	}
};