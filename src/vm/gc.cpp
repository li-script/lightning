#include <vm/gc.hpp>
#include <vm/state.hpp>
#include <vm/table.hpp>
#include <vm/string.hpp>
#include <span>
#include <cmath>

namespace li::gc {
	static constexpr uint32_t small_class_count   = 8;
	static constexpr uint32_t max_realistic_alloc = 2 * 1024 * 1024 / chunk_size;
	static int size_class_of(uint32_t nchunks, bool for_alloc) {
		if (nchunks <= small_class_count) {
			return nchunks - 1;
		}
		nchunks -= small_class_count;
		float f = std::min(1.0f, sqrtf(float(nchunks)) * sqrtf(1.0f / max_realistic_alloc));
		if (for_alloc)
         return small_class_count + (int) roundf((num_size_classes - small_class_count - 1) * f);
      else
         return small_class_count + (int) floorf((num_size_classes - small_class_count - 1) * f);
	}

	void header::gc_init(page* p, vm* L, msize_t clen, value_type t) {
		gc_type        = t;
		num_chunks     = clen;
		indep_or_free  = false;
		page_offset    = (uintptr_t(this) - uintptr_t(p)) >> 12;
		stage          = L ? L->stage : 0;
	}
	bool header::gc_tick(stage_context s, bool weak) {
		LI_ASSERT(!is_free());

		// If already iterated, skip.
		//
		if (stage == s) [[likely]] {
			return true;
		}

		// Update stage, recurse.
		//
		stage = s;
		switch (gc_type) {
			case type_table:    traverse(s, (table*) this);          break;
			case type_array:    traverse(s, (array*) this);          break;
			case type_userdata: traverse(s, (userdata*) this);       break;
			case type_function: traverse(s, (function*) this);       break;
			case type_proto:    traverse(s, (function_proto*) this); break;
			default: break;
		}

		// Increment counter.
		//
		get_page()->alive_objects++;
		return true;
	}

	std::pair<page*, header*> state::allocate_uninit(vm* L, msize_t clen) {
		LI_ASSERT(clen != 0);

		// Fast path for small sizes.
		//
		if (clen <= small_class_count) [[likely]] {
			auto& fl = free_lists[clen - 1];
			if (fl) [[likely]] {
				auto* it          = std::exchange(fl, fl->get_next_free());
				auto* page        = it->get_page();
				it->gc_type       = type_gc_uninit;
				it->indep_or_free = false;
				page->num_objects++;
				return {page, it};
			}
		}

		// Try allocating from a free list.
		//
		auto* free_list = &free_lists[size_class_of(clen, true)];
		if (!*free_list && free_list != &free_lists[num_size_classes-1])
			free_list++;

		header* prev = nullptr;
		header* it   = *free_list;
		while (it) {
			if (it->num_chunks < clen) [[unlikely]] {
				prev = it;
				it   = it->get_next_free();
				continue;
			}

			// Unlink the entry.
			//
			if (prev) {
				prev->set_next_free(it->get_next_free());
			} else {
				*free_list = it->get_next_free();
			}

			// Get the page and change the type.
			//
			auto* page        = it->get_page();
			it->gc_type       = type_gc_uninit;
			it->indep_or_free = false;
			page->num_objects++;

			// If we didn't allocate all of the space, re-insert into the free-list.
			//
			if (msize_t leftover = it->num_chunks - clen) {
				it->num_chunks     = clen;
				auto& free_list    = free_lists[size_class_of(leftover, false)];
				auto* fh2          = it->next();
				fh2->gc_type       = type_gc_free;
				fh2->indep_or_free = true;
				fh2->num_chunks    = leftover;
				fh2->page_offset   = (uintptr_t(fh2) - uintptr_t(page)) >> 12;
				fh2->set_next_free(free_list);
				free_list = fh2;
			}

			// Return the result.
			//
			return {page, it};
		}

		// Find a page with enough size to fit our object or allocate one.
		//
		page* pg = initial_page->next;
		if (!pg->check_space(clen)) {
			pg = add_page<false>(L, size_t(clen) << chunk_shift);
			if (!pg)
				return {nullptr, nullptr};
		}

		// Increment GC debt, return the result.
		//
		debt += clen;
		if (debt >= max_debt)
			ticks = 0;
		else if (debt >= min_debt)
			ticks = interval;
		return {pg, pg->alloc_arena(clen)};
	}
	std::pair<page*, header*> state::allocate_uninit_ex(vm* L, msize_t clen) {
		LI_ASSERT(clen != 0);

		// Try allocating from the free list.
		//
		header* it   = ex_free_list;
		header* prev = nullptr;
		while (it) {
			if (it->num_chunks >= clen) {
				// Unlink the entry.
				//
				if (prev) {
					prev->set_next_free(it->get_next_free());
				} else {
					ex_free_list = it->get_next_free();
				}

				// Get the page and change the type.
				//
				auto* page        = it->get_page();
				it->gc_type       = type_gc_uninit;
				it->indep_or_free = false;
				page->num_objects++;

				// If we didn't allocate all of the space, re-insert into the free-list.
				//
				if (msize_t leftover = it->num_chunks - clen) {
					it->num_chunks     = clen;
					auto& free_list    = ex_free_list;
					auto* fh2          = it->next();
					fh2->gc_type       = type_gc_free;
					fh2->indep_or_free = true;
					fh2->num_chunks    = leftover;
					fh2->page_offset   = (uintptr_t(fh2) - uintptr_t(page)) >> 12;
					fh2->set_next_free(free_list);
					free_list = fh2;
				}

				// Return the result.
				//
				return {page, it};
			} else {
				prev = it;
				it   = it->get_next_free();
			}
		}

		// Find a page with enough size to fit our object or allocate one.
		//
		page* pg = for_each_ex([&](page* p, bool exec) LI_INLINE { return p->check_space(clen); });
		if (!pg) {
			pg = add_page<true>(L, size_t(clen) << chunk_shift);
			if (!pg)
				return {nullptr, nullptr};
		}
		return {pg, pg->alloc_arena(clen)};
	}
	void state::free(vm* L, header* o, bool within_gc) {
		LI_ASSERT_MSG("Double free", !o->is_free());

		// Decrement counters.
		//
		auto* page = o->get_page();
		if (!within_gc)
			page->alive_objects--;
		page->num_objects--;

		// Run destructor if relevant.
		//
		switch (o->gc_type) {
			case type_table:    destroy(L, (table*) o);    break;
			case type_array:    destroy(L, (array*) o);    break;
			case type_userdata: destroy(L, (userdata*) o); break;
			default: break;
		}
#if LI_DEBUG
		memset(o + 1, 0xCC, o->object_bytes());
#endif

		// Insert into the free list or adjust arena.
		//
		bool at_arena_end = o->next() == page->end();
		if (at_arena_end && !page->is_exec) {
			at_arena_end = page == initial_page->next;
		}
		if (!at_arena_end) {
			auto& free_list  = page->is_exec ? ex_free_list : free_lists[size_class_of(o->num_chunks, false)];
			o->gc_type       = type_gc_free;
			o->indep_or_free = true;
			o->set_next_free(free_list);
			free_list = o;
		} else {
			page->next_chunk -= o->num_chunks;
		}
	}

	static void traverse_live(vm* L, stage_context s) {
		// Stack.
		//
		for (auto& e : std::span{L->stack, L->stack_top}) {
			if (e.is_gc())
				e.as_gc()->gc_tick(s);
		}

		// Globals.
		//
		L->modules->gc_tick(s);
		if (L->repl_scope) {
			L->repl_scope->gc_tick(s);
		}

		// Strings.
		//
		((header*) L->empty_string)->gc_tick(s);
		((header*) L->strset)->gc_tick(s);
	}

	void state::close(vm* L) {
		// Clear stack and globals.
		//
		L->stack_top = L->stack;
		L->modules->mask = 0;
		if (L->repl_scope) {
			L->repl_scope->mask = 0;
		}

		// GC.
		//
		L->gc.collect(L);

		// Free all pages.
		//
		auto* alloc = alloc_fn;
		void* actx  = alloc_ctx;
		auto* exhead = initial_ex_page;
		auto* head  = initial_page;
		if (exhead) {
			for (auto it = exhead->next; it != exhead;) {
				auto p = std::exchange(it, it->next);
				alloc(actx, p, p->num_pages, true);
			}
		}
		for (auto it = head->next; it != head;) {
			auto p = std::exchange(it, it->next);
			alloc(actx, p, p->num_pages, false);
		}
		alloc(actx, head, head->num_pages, false);
		alloc(actx, actx, 0, false);
	}

	LI_COLD void state::collect(vm* L) {
		if (suspend) [[unlikely]]
			return;

		// Reset GC tick.
		//
		ticks = min_debt ? INT64_MAX : interval;
		debt  = 0;
		collect_counter++;

		// Clear alive counter in all pages.
		//
		for_each([](page* p, bool x){
			p->alive_objects = p->num_indeps;
			return false;
		});

		// Mark all alive objects.
		//
		L->stage ^= 1;
		stage_context ms{bool(L->stage)};
		traverse_live(L, ms);

		// Free all dead objects.
		//
		page* dead_page_list = nullptr;
		for_each([&](page* it, bool ex) LI_INLINE {
			if (it->alive_objects != it->num_objects) {
				it->for_each([&](header* obj) LI_INLINE {
					if (!obj->indep_or_free && obj->stage != ms) {
						free(L, obj, true);
					}
					return false;
				});

				if (it->alive_objects || greedy)
					return false;
				util::unlink(it);
				if (!dead_page_list) {
					it->next       = nullptr;
					dead_page_list = it;
				} else {
					it->next       = dead_page_list;
					dead_page_list = it;
				}
			}
			return false;
		});

		// Sweep dead references.
		//
		strset_sweep(L, ms);

		// If we can free any pages, do so.
		//
		if (dead_page_list) [[unlikely]] {
			// Fix free lists.
			//
			for (size_t i = 0; i != free_lists.size(); i++) {
				header** prev = &free_lists[i];
				for (auto it = *prev; it;) {
					auto* next = it->get_next_free();

					if (!it->get_page()->alive_objects) {
						*prev = it->get_next_free();
					} else {
						prev = &it->ref_next_free();
					}
					it = next;
				}
			}

			// Deallocate.
			//
			while (dead_page_list) {
				auto i = std::exchange(dead_page_list, dead_page_list->next);
				alloc_fn(alloc_ctx, i, i->num_pages, i->is_exec);
			}
		}
	}

	void header::acquire() {
		auto* page = get_page();
		LI_ASSERT_MSG("Double acquire", !is_independent());
		indep_or_free = true;
		page->num_indeps++;
		page->num_objects--;
	}
	void header::release(vm* L) {
		auto* page = get_page();
		LI_ASSERT_MSG("Double release", is_independent());
		indep_or_free = false;
		stage       = L->stage;
		page->num_indeps--;
		page->num_objects++;
	}

	// Implement mem_ functions.
	//
	void mem_release(vm* L, void* p) {
		auto* h = std::prev((header*) p);
		LI_ASSERT(h->gc_type == type_gc_private);
		h->release(L);
	}
	void mem_acquire(void* p) {
		auto* h    = std::prev((header*) p);
		LI_ASSERT(h->gc_type == type_gc_private);
		h->acquire();
	}
	void* mem_alloc(vm* L, size_t n, bool independent) {
		msize_t length    = (msize_t) (chunk_ceil(n + sizeof(header)) >> chunk_shift);
		auto [page, base] = L->gc.allocate_uninit(L, length);
		base->gc_init(page, L, length, type_gc_private);
		if (independent) {
			base->acquire();
		}
		return base + 1;
	}
	void mem_free(vm* L, void* p) {
		if (p) {
			L->gc.free(L, std::prev((header*) p));
		}
	}
	void* mem_realloc(vm* L, void* p, size_t n, bool independent) {
		if (!p) {
			if (!n)
				return nullptr;
			return mem_alloc(L, n, independent);
		} else {
			auto* h = std::prev((header*) p);
			if (h->object_bytes() >= n)
				return p;
			void* np = mem_alloc(L, n, independent);
			memcpy(np, p, h->object_bytes());
			mem_free(L, p);
			return np;
		}
	}
};