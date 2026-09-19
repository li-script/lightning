#include <ir/proc.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace li::ir {
	namespace {
		using block_index_map     = std::unordered_map<const basic_block*, size_t>;
		constexpr size_t no_index = std::numeric_limits<size_t>::max();

		std::vector<basic_block*> reachable_rpo(const procedure* proc) {
			auto* entry = proc->get_entry();
			if (!entry)
				return {};

			struct frame {
				basic_block* block;
				size_t       next_successor;
			};

			const auto                mark = ++proc->next_visited_mark;
			std::vector<frame>        stack;
			std::vector<basic_block*> postorder;
			stack.reserve(proc->basic_blocks.size());
			postorder.reserve(proc->basic_blocks.size());

			entry->visited = mark;
			stack.push_back({entry, 0});
			while (!stack.empty()) {
				auto& current = stack.back();
				if (current.next_successor != current.block->successors.size()) {
					auto* successor = current.block->successors[current.next_successor++];
					if (successor->visited != mark) {
						successor->visited = mark;
						stack.push_back({successor, 0});
					}
				} else {
					postorder.push_back(current.block);
					stack.pop_back();
				}
			}

			std::reverse(postorder.begin(), postorder.end());
			return postorder;
		}

		block_index_map index_blocks(const std::vector<basic_block*>& blocks) {
			block_index_map indices;
			indices.reserve(blocks.size());
			for (size_t i = 0; i != blocks.size(); ++i)
				indices.emplace(blocks[i], i);
			return indices;
		}

		size_t intersect(size_t left, size_t right, const std::vector<size_t>& idom) {
			while (left != right) {
				while (left > right)
					left = idom[left];
				while (right > left)
					right = idom[right];
			}
			return left;
		}

		template<typename VisitPredecessors>
		std::vector<size_t> find_immediate_dominators(size_t count, VisitPredecessors&& visit_predecessors) {
			std::vector<size_t> idom(count, no_index);
			if (!count)
				return idom;
			idom[0] = 0;

			bool changed;
			do {
				changed = false;
				for (size_t block = 1; block != count; ++block) {
					size_t next_idom = no_index;
					visit_predecessors(block, [&](size_t predecessor) {
						if (idom[predecessor] == no_index)
							return;
						next_idom = next_idom == no_index ? predecessor : intersect(predecessor, next_idom, idom);
					});
					if (next_idom != no_index && idom[block] != next_idom) {
						idom[block] = next_idom;
						changed     = true;
					}
				}
			} while (changed);
			return idom;
		}

		template<typename SetInterval>
		void number_dominator_tree(const std::vector<size_t>& idom, SetInterval&& set_interval) {
			if (idom.empty())
				return;

			std::vector<size_t> first_child(idom.size(), no_index);
			std::vector<size_t> next_sibling(idom.size(), no_index);
			for (size_t block = 1; block != idom.size(); ++block) {
				LI_ASSERT(idom[block] != no_index);
				next_sibling[block]      = first_child[idom[block]];
				first_child[idom[block]] = block;
			}

			struct frame {
				size_t block;
				size_t next_child;
			};
			LI_ASSERT(idom.size() <= std::numeric_limits<msize_t>::max() / 2);
			msize_t            clock = 0;
			std::vector<frame> stack;
			stack.reserve(idom.size());
			stack.push_back({0, first_child[0]});
			set_interval(0, ++clock, 0);
			while (!stack.empty()) {
				auto& current = stack.back();
				if (current.next_child != no_index) {
					const size_t child = current.next_child;
					current.next_child = next_sibling[child];
					set_interval(child, ++clock, 0);
					stack.push_back({child, first_child[child]});
				} else {
					set_interval(current.block, 0, ++clock);
					stack.pop_back();
				}
			}
		}
	}

	void procedure::ensure_dominators() const {
		if (dominator_revision == cfg_revision)
			return;

		for (auto& block : basic_blocks) {
			block->dominator_pre  = 0;
			block->dominator_post = 0;
		}

		auto blocks = reachable_rpo(this);
		if (!blocks.empty()) {
			auto indices = index_blocks(blocks);
			auto idom    = find_immediate_dominators(blocks.size(), [&](size_t block, auto&& visit) {
				for (auto* predecessor : blocks[block]->predecessors) {
					auto it = indices.find(predecessor);
					if (it != indices.end())
						visit(it->second);
				}
			});
			number_dominator_tree(idom, [&](size_t block, msize_t pre, msize_t post) {
				if (pre)
					blocks[block]->dominator_pre = pre;
				if (post)
					blocks[block]->dominator_post = post;
			});
		}

		dominator_revision = cfg_revision;
	}

	void procedure::ensure_postdominators() const {
		if (postdominator_revision == cfg_revision)
			return;

		for (auto& block : basic_blocks) {
			block->postdominator_pre  = 0;
			block->postdominator_post = 0;
		}

		auto forward_blocks = reachable_rpo(this);
		if (forward_blocks.empty()) {
			postdominator_revision = cfg_revision;
			return;
		}

		auto                 forward_indices = index_blocks(forward_blocks);
		std::vector<uint8_t> can_reach_exit(forward_blocks.size(), false);
		std::vector<size_t>  worklist;
		worklist.reserve(forward_blocks.size());
		for (size_t i = 0; i != forward_blocks.size(); ++i) {
			if (forward_blocks[i]->successors.empty()) {
				can_reach_exit[i] = true;
				worklist.push_back(i);
			}
		}
		for (size_t cursor = 0; cursor != worklist.size(); ++cursor) {
			for (auto* predecessor : forward_blocks[worklist[cursor]]->predecessors) {
				auto found = forward_indices.find(predecessor);
				if (found != forward_indices.end() && !can_reach_exit[found->second]) {
					can_reach_exit[found->second] = true;
					worklist.push_back(found->second);
				}
			}
		}

		// The synthetic exit has an edge to each real exit in the reversed CFG.
		// Blocks that cannot reach a real exit are also attached directly to it:
		// this keeps no-exit cycles from acquiring arbitrary postdominators.
		//
		std::vector<uint8_t> synthetic_successor(forward_blocks.size(), false);
		for (size_t i = 0; i != forward_blocks.size(); ++i)
			synthetic_successor[i] = forward_blocks[i]->successors.empty() || !can_reach_exit[i];

		struct frame {
			basic_block* block;
			size_t       next_predecessor;
		};
		const auto                mark = ++next_visited_mark;
		std::vector<frame>        stack;
		std::vector<basic_block*> postorder;
		stack.reserve(forward_blocks.size());
		postorder.reserve(forward_blocks.size() + 1);
		for (size_t root = 0; root != forward_blocks.size(); ++root) {
			if (!synthetic_successor[root] || forward_blocks[root]->visited == mark)
				continue;
			forward_blocks[root]->visited = mark;
			stack.push_back({forward_blocks[root], 0});
			while (!stack.empty()) {
				auto& current = stack.back();
				if (current.next_predecessor != current.block->predecessors.size()) {
					auto* predecessor = current.block->predecessors[current.next_predecessor++];
					if (forward_indices.contains(predecessor) && predecessor->visited != mark) {
						predecessor->visited = mark;
						stack.push_back({predecessor, 0});
					}
				} else {
					postorder.push_back(current.block);
					stack.pop_back();
				}
			}
		}

		postorder.push_back(nullptr);
		std::reverse(postorder.begin(), postorder.end());
		auto post_indices = index_blocks(postorder);
		auto idom         = find_immediate_dominators(postorder.size(), [&](size_t block, auto&& visit) {
			auto* current       = postorder[block];
			auto  forward_index = forward_indices.find(current);
			LI_ASSERT(forward_index != forward_indices.end());
			if (synthetic_successor[forward_index->second])
				visit(0);
			for (auto* successor : current->successors) {
				auto found = post_indices.find(successor);
				if (found != post_indices.end())
					visit(found->second);
			}
		});
		number_dominator_tree(idom, [&](size_t block, msize_t pre, msize_t post) {
			if (!block)
				return;
			if (pre)
				postorder[block]->postdominator_pre = pre;
			if (post)
				postorder[block]->postdominator_post = post;
		});

		postdominator_revision = cfg_revision;
	}

	bool basic_block::dom(const basic_block* n) const {
		if (!n || proc != n->proc)
			return false;
		if (this == n)
			return true;
		proc->ensure_dominators();
		return dominator_pre != 0 && n->dominator_pre != 0 && dominator_pre <= n->dominator_pre && n->dominator_post <= dominator_post;
	}

	bool basic_block::postdom(const basic_block* n) const {
		if (!n || proc != n->proc)
			return false;
		if (this == n)
			return true;
		proc->ensure_postdominators();
		return postdominator_pre != 0 && n->postdominator_pre != 0 && postdominator_pre <= n->postdominator_pre && n->postdominator_post <= postdominator_post;
	}

	// Returns true if this block can reach the block.
	//
	bool basic_block::check_path(const basic_block* to) const {
		return proc->bfs([&](const basic_block* b) { return b == to; }, this);
	}
};