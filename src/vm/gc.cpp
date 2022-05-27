#include <vm/gc.hpp>
#include <vm/state.hpp>
#include <vm/table.hpp>
#include <vm/string.hpp>
#include <span>

#if defined(_CPPRTTI)
	#define HAS_RTTI _CPPRTTI
#elif defined(__GXX_RTTI)
	#define HAS_RTTI __GXX_RTTI
#elif defined(__has_feature)
	#define HAS_RTTI __has_feature(cxx_rtti)
#else
	#define HAS_RTTI 0
#endif
#if HAS_RTTI
	#include <typeinfo>
#endif

namespace li::gc {
	void header::gc_init(page* p, vm* L, uint32_t clen, bool trv) {
		num_chunks     = clen;
		traversable    = trv;
		page_offset    = (uintptr_t(this) - uintptr_t(p)) >> 12;
		stage          = L ? L->stage : 0;
	}
	bool header::gc_tick(stage_context s, bool weak) {
		// If dead, skip.
		//
		if (is_free()) [[unlikely]] {
			return false;
		}

		// If already iterated, skip.
		//
		if (stage == s.next_stage) [[likely]] {
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

	
	// Allocates an uninitialized chunk.
	//
	std::pair<page*, void*> state::allocate_uninit(vm* L, uint32_t clen) {
		//printf("allocating %llu\n", size_t(clen) << chunk_shift);

		// Try allocating from a free list.
		//
		auto try_alloc_class = [&](size_t scl, bool excess) LI_INLINE -> std::pair<page*, void*> {
			free_header* it   = free_lists[scl];
			free_header* prev = nullptr;
			while (it) {
				if (excess || it->num_chunks >= clen) {
					// Unlink the entry.
					//
					if (prev) {
						prev->set_next(it->next());
					} else {
						free_lists[scl] = it->next();
					}

					// Get the page.
					//
					auto* page = ((header*) it)->get_page();

					// If we didn't allocate all of the space, re-insert into the free-list.
					//
					if (uint32_t leftover = it->num_chunks - clen) {
						it->num_chunks   = clen;
						auto* h2         = ((header*) it)->next();
						auto* fh2        = h2->get_free_header();
						fh2->valid       = true;
						fh2->next_free   = 0;
						fh2->num_chunks  = leftover;
						fh2->page_offset = (uintptr_t(fh2) - uintptr_t(page)) >> 12;

						// Insert into the free list.
						//
						if (clen != 1) {
							auto& free_list = free_lists[size_class_of(fh2->num_chunks)];
							fh2->set_next(free_list);
							free_list = fh2;
						}
					}

					// Return the result.
					//
					return {page, it};
				} else {
					prev = it;
					it   = it->next();
				}
			}
			return {nullptr, nullptr};
		};
		{
			size_t size_class = size_class_of(clen);
			auto   alloc      = try_alloc_class(size_class, false);
			if (!alloc.second && ++size_class != std::size(size_classes)) {
				alloc = try_alloc_class(size_class, true);
			}
			if (alloc.second)
				return alloc;
		}

		// Find a page with enough size to fit our object or allocate one.
		//
		auto* pg = for_each([&](page* p) { return p->check_space(clen); });
		if (!pg) {
			pg = add_page(L, size_t(clen) << chunk_shift, false);
			if (!pg)
				return {nullptr, nullptr};
		}

		// Increment GC depth, return the result.
		//
		debt += clen;
		return {pg, pg->allocate_uninit(clen)};
	}
	void state::free(header* o, bool internal) {
		LI_ASSERT_MSG("Double free", !o->is_free());

#if 0
		if (auto t = o->gc_identify()) {
			//printf("freeing: ");
			//any v(std::in_place, mix_value(t, (uint64_t) o));
			//v.print();
			//if (t == type_table) {
			//	printf(" [");
			//	for (auto& [k, v] : *v.as_tbl()) {
			//		if (k.is(type_string)) {
			//			printf("%s, ", k.as_str()->c_str());
			//		}
			//	}
			//	printf("]");
			//}
			//
			//putchar('\n');
		} else {
#if HAS_RTTI
			//printf("freeing: opaque %p (%s)\n", o, typeid(*o).name());
#else
			//printf("freeing: opaque %p\n", o);
#endif
		}
#endif

		// If call was not made internally, decrement alive counter.
		//
		if (!internal) {
			o->get_page()->alive_objects--;
		}

		// Set the object free.
		//
		uint32_t num_chunks  = o->num_chunks;
		uint32_t page_offset = o->page_offset;
#ifndef __EMSCRIPTEN__
		std::destroy_at(o);
#endif
		auto* fh        = o->get_free_header();
		fh->valid       = true;
		fh->next_free   = 0;
		fh->num_chunks  = num_chunks;
		fh->page_offset = page_offset;
#if LI_DEBUG
		memset(fh + 1, 0xCC, (num_chunks << chunk_shift) - sizeof(header));
#endif

		// Insert into the free list.
		//
		auto& free_list = free_lists[size_class_of(num_chunks)];
		fh->set_next(free_list);
		free_list = fh;
	}

	static void traverse_live(vm* L, stage_context sweep, bool include_weak) {
		// Stack.
		//
		for (auto& e : std::span{L->stack, L->stack_top}) {
			if (e.is_gc())
				e.as_gc()->gc_tick(sweep);
		}
		std::prev(((header*) L->stack))->gc_tick(sweep);

		// Globals.
		//
		L->globals->gc_tick(sweep);

		// Strings.
		//
		((header*) L->empty_string)->gc_tick(sweep);
		if (!include_weak)
			traverse_string_set(L, sweep);
	}

	LI_COLD void state::collect(vm* L) {
		// Reset GC tick.
		//
		ticks = gc_interval;
		debt  = 0;

		// Clear alive counter in all pages.
		//
		initial_page->alive_objects = 1; // vm
		for (auto it = initial_page->next; it != initial_page; it = it->next) {
			it->alive_objects = 0;
		}

		// TODO: Free excess pages, NYI because cba fixing free list.
		//

		// Mark all alive objects.
		//
		stage_context ms{++L->stage};
		traverse_live(L, ms, false);

		// Free all dead objects.
		//
		//page* free_list = nullptr;
		for_each([&](page* it) {
			if (it->alive_objects != it->num_objects) {
				it->for_each([&](header* obj) {
					if (!obj->is_free() && obj->stage != ms.next_stage)
						free(obj, true);
					return false;
				});

				//if (!it->alive_objects) {
				//	util::unlink(it);
				//	if (!free_list) {
				//		it->next  = nullptr;
				//		free_list = it;
				//	} else {
				//		it->next  = free_list;
				//		free_list = it;
				//	}
				//}

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
		//while (free_list) {
		//	auto i = std::exchange(free_list, free_list->next);
		//	alloc_fn(alloc_ctx, i, i->num_pages, false);
		//}
		// TODO: Table/array/stack/strset shrinking.
	}
};