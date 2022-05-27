#include <vm/gc.hpp>
#include <vm/state.hpp>
#include <vm/table.hpp>
#include <span>


namespace li::gc {

	struct sweep_state {
		uint8_t next_stage;
		size_t  counter = 0;
	};

	void header::gc_init(vm* L) { stage = L ? L->gc.stage : 0; }
	bool header::gc_tick(sweep_state& s, bool weak) {
		if (stage != s.next_stage) {
			stage = s.next_stage;
			if (traversable)
				gc_traverse(s);

			printf("marked gc object %llu: %p\n", s.counter++, this);
		}
		return true;
	}

	void state::collect(vm* L) {
		stage = !stage;
		sweep_state sweep{stage};
		
		for (auto it = page_list_head.next; it != &page_list_head; it = it->next) {
			printf("gc page %p has %llu objects\n", it, it->num_objects);
		}


		// Stack.
		for (auto& e : std::span{L->stack, L->stack_top}) {
			if (e.is_gc())
				e.as_gc()->gc_tick(sweep);
		}

		// Globals.
		L->globals->gc_traverse(sweep);

		// Strings.
		traverse_string_set(L, sweep);
	}
};