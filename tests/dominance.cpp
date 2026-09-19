#include <ir/proc.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace {
	using li::ir::basic_block;
	using li::ir::procedure;

	void require(bool condition, std::string_view message) {
		if (!condition)
			throw std::runtime_error(std::string(message));
	}

	void connect(procedure& proc, basic_block* from, basic_block* to) { proc.add_jump(from, to); }

	void diamond_probe() {
		procedure proc(nullptr, nullptr);
		auto*     entry = proc.add_block();
		auto*     left  = proc.add_block();
		auto*     right = proc.add_block();
		auto*     join  = proc.add_block();
		auto*     exit  = proc.add_block();
		connect(proc, entry, left);
		connect(proc, entry, right);
		connect(proc, left, join);
		connect(proc, right, join);
		connect(proc, join, exit);

		require(entry->dom(left), "diamond entry must dominate left");
		require(entry->dom(exit), "diamond entry must dominate exit");
		require(join->dom(exit), "diamond join must dominate exit");
		require(!left->dom(join), "one diamond arm must not dominate join");
		require(!join->dom(entry), "diamond join must not dominate entry");

		require(join->postdom(entry), "diamond join must postdominate entry");
		require(join->postdom(left), "diamond join must postdominate left");
		require(join->postdom(right), "diamond join must postdominate right");
		require(exit->postdom(entry), "sole exit must postdominate diamond entry");
		require(!left->postdom(entry), "one diamond arm must not postdominate entry");
	}

	void backedge_probe() {
		procedure proc(nullptr, nullptr);
		auto*     entry  = proc.add_block();
		auto*     header = proc.add_block();
		auto*     body   = proc.add_block();
		auto*     exit   = proc.add_block();
		connect(proc, entry, header);
		connect(proc, header, body);
		connect(proc, body, header);
		connect(proc, header, exit);

		require(header->dom(body), "loop header must dominate body");
		require(header->dom(exit), "loop header must dominate loop exit");
		require(!body->dom(header), "loop body must not dominate header");
		require(header->postdom(entry), "loop header must postdominate its entry");
		require(exit->postdom(header), "loop exit must postdominate header on terminating paths");
		require(!body->postdom(header), "loop body must not postdominate exiting header");
	}

	void multi_exit_and_no_exit_probe() {
		procedure proc(nullptr, nullptr);
		auto*     entry          = proc.add_block();
		auto*     normal_exit    = proc.add_block();
		auto*     exception_exit = proc.add_block();
		connect(proc, entry, normal_exit);
		connect(proc, entry, exception_exit);

		require(entry->dom(normal_exit), "entry must dominate normal exit");
		require(entry->dom(exception_exit), "entry must dominate exception exit");
		require(!normal_exit->postdom(entry), "normal exit must not postdominate exception path");
		require(!exception_exit->postdom(entry), "exception exit must not postdominate normal path");

		auto* spin_a = proc.add_block();
		auto* spin_b = proc.add_block();
		connect(proc, entry, spin_a);
		connect(proc, spin_a, spin_b);
		connect(proc, spin_b, spin_a);

		require(!normal_exit->postdom(entry), "real exit must not postdominate a path into a no-exit cycle");
		require(!spin_a->postdom(spin_b), "no-exit cycle must not gain an arbitrary postdominator");
		require(!spin_b->postdom(spin_a), "no-exit cycle postdominance must be conservative");
		require(spin_a->postdom(spin_a), "a no-exit block must postdominate itself");
	}

	void edit_invalidation_probe() {
		procedure proc(nullptr, nullptr);
		auto*     entry  = proc.add_block();
		auto*     middle = proc.add_block();
		auto*     exit   = proc.add_block();
		auto*     dead_a = proc.add_block();
		auto*     dead_b = proc.add_block();
		connect(proc, entry, middle);
		connect(proc, middle, exit);
		connect(proc, dead_a, dead_b);
		connect(proc, dead_b, dead_a);

		// Populate both caches before changing the graph.
		require(middle->dom(exit), "middle must initially dominate exit");
		require(exit->postdom(entry), "exit must initially postdominate entry");
		require(!dead_a->dom(dead_b), "unreachable cycle must not acquire arbitrary dominance");
		require(!dead_a->postdom(dead_b), "unreachable cycle must not acquire arbitrary postdominance");
		require(dead_a->dom(dead_a), "unreachable block must dominate itself");

		connect(proc, entry, exit);
		require(!middle->dom(exit), "new bypass edge must invalidate cached dominance");
		proc.del_jump(entry, exit);
		require(middle->dom(exit), "deleted bypass edge must invalidate cached dominance");

		auto* alternate_exit = proc.add_block();
		connect(proc, entry, alternate_exit);
		require(!exit->postdom(entry), "new exit edge must invalidate cached postdominance");

		connect(proc, entry, dead_a);
		require(dead_a->dom(dead_b), "newly reachable cycle dominance must be rebuilt");
		require(!dead_a->postdom(dead_b), "reachable no-exit cycle postdominance must remain conservative");

		connect(proc, dead_b, alternate_exit);
		require(alternate_exit->postdom(dead_a), "new cycle exit must invalidate no-exit postdominance state");
	}

	void parallel_edge_probe() {
		procedure proc(nullptr, nullptr);
		auto*     entry = proc.add_block();
		auto*     join  = proc.add_block();
		connect(proc, entry, join);
		connect(proc, entry, join);

		require(entry->dom(join), "parallel edges must preserve dominance");
		require(join->postdom(entry), "parallel edges to one target must preserve postdominance");
	}
}

int main() {
	diamond_probe();
	backedge_probe();
	multi_exit_and_no_exit_probe();
	edit_invalidation_probe();
	parallel_edge_probe();
	return 0;
}
