#include <vm/gc.hpp>
#include <vm/state.hpp>
#include <vm/table.hpp>
#include <span>


namespace li::gc {
	void header::gc_init(page* p, vm* L, uint32_t qlen, bool traversable) {
		size_in_qwords = qlen;
		traversable    = traversable;
		page_offset    = (uintptr_t(this) - uintptr_t(p)) >> 12;
		stage          = L ? L->stage : 0;
		free           = false;
	}
	bool header::gc_tick(stage_context s, bool weak) {
		// If dead, skip.
		//
		if (free) [[unlikely]] {
			return false;
		}

		// If already iterated, skip.
		//
		if (stage == s.next_stage) {
			return true;
		}

		// Update stage, recurse.
		//
		stage = s.next_stage;
		if (traversable)
			gc_traverse(s);

		// Increment counter.
		//
		get_page()->alive_objects++;
		return true;
	}
	void header::gc_free() {
		// TODO: Insert to free list
		free = true;
	}

	static void traverse_live(vm* L, stage_context sweep, bool include_weak) {
		// Stack.
		//
		for (auto& e : std::span{L->stack, L->stack_top}) {
			if (e.is_gc())
				e.as_gc()->gc_tick(sweep);
		}

		// Globals.
		//
		L->globals->gc_traverse(sweep);

		// Strings.
		//
		((header*) L->empty_string)->gc_tick(sweep);
		if (!include_weak)
			traverse_string_set(L, sweep);
	}

	void state::collect(vm* L) {
		// Clear alive counter in all pages.
		//
		initial_page->alive_objects = 1; // vm
		for (auto it = initial_page->next; it != initial_page; it = it->next) {
			it->alive_objects = 0;
		}

		// Mark all alive objects.
		//
		stage_context ms{++L->stage};
		traverse_live(L, ms, false);

		// Free all dead objects.
		//
		page* free_list = nullptr;
		for_each([&](page* it) {
			if (it->alive_objects != it->num_objects) {

				it->for_each([&](header* obj) {
					if (obj->stage != ms.next_stage)
						obj->gc_free();
					return false;
				});

				if (!it->alive_objects) {
					util::unlink(it);
					if (!free_list) {
						free_list = it;
					} else {
						it->next  = free_list;
						free_list = it;
					}
				}

				it->alive_objects = 0; // Will be incremented again by next step.
			}
			return false;
		});

		// Sweep dead references.
		//
		stage_context ss{++L->stage};
		traverse_live(L, ss, true);

		// If we can free any pages, do so.
		//
		while (free_list) {
			auto i = std::exchange(free_list, free_list->next);
			alloc_fn(alloc_ctx, i, i->num_pages, false);
		}
		// TODO: Table/array/stack/strset shrinking.
	}
};