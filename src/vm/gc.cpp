#include <vm/gc.hpp>
#include <vm/state.hpp>
#include <vm/table.hpp>
#include <vm/string.hpp>
#include <span>

namespace li::gc {
	void header::gc_init(page* p, vm* L, uint32_t clen, value_type t) {
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
		if (gc_type <= type_gc_last_traversable) {
			if (gc_type == type_array)
				traverse(s, (array*) this);
			else if (gc_type == type_table)
				traverse(s, (table*) this);
			else if (gc_type == type_function)
				traverse(s, (function*) this);
		}

		// Increment counter.
		//
		get_page()->alive_objects++;
		return true;
	}

	std::pair<page*, header*> state::allocate_uninit(vm* L, uint32_t clen) {
		LI_ASSERT(clen != 0);

		// Try allocating from a free list.
		//
		auto try_alloc_class = [&](size_t scl, bool excess) LI_INLINE -> std::pair<page*, header*> {
			header* it   = free_lists[scl];
			header* prev = nullptr;
			while (it) {
				if (excess || it->num_chunks >= clen) {
					// Unlink the entry.
					//
					if (prev) {
						prev->set_next_free(it->get_next_free());
					} else {
						free_lists[scl] = it->get_next_free();
					}

					// Get the page and change the type.
					//
					auto* page  = it->get_page();
					it->gc_type = type_gc_uninit;
					it->indep_or_free = false;
					page->num_objects++;

					// If we didn't allocate all of the space, re-insert into the free-list.
					//
					if (uint32_t leftover = it->num_chunks - clen) {
						it->num_chunks   = clen;
						auto& free_list  = free_lists[size_class_of(leftover)];
						auto* fh2        = it->next();
						fh2->gc_type     = type_gc_free;
						fh2->indep_or_free = true;
						fh2->num_chunks  = leftover;
						fh2->page_offset = (uintptr_t(fh2) - uintptr_t(page)) >> 12;
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
		auto* pg = for_each_rw([&](page* p, bool) { return p->check_space(clen); });
		if (!pg) {
			pg = add_page<false>(L, size_t(clen) << chunk_shift);
			if (!pg)
				return {nullptr, nullptr};
		}

		// Increment GC depth, return the result.
		//
		debt += clen;
		return {pg, pg->alloc_arena(clen)};
	}
	std::pair<page*, header*> state::allocate_uninit_ex(vm* L, uint32_t clen) {
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
				if (uint32_t leftover = it->num_chunks - clen) {
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
		auto* pg = for_each_ex([&](page* p, bool exec) { return p->check_space(clen); });
		if (!pg) {
			pg = add_page<true>(L, size_t(clen) << chunk_shift);
			if (!pg)
				return {nullptr, nullptr};
		}

		// Increment GC depth, return the result.
		//
		debt += clen;
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
		if (is_type_traitful(o->gc_type)) {
			((traitful_node<>*) o)->gc_destroy(L);
		}
#if LI_DEBUG
		memset(o + 1, 0xCC, o->object_bytes());
#endif

		// Insert into the free list or adjust arena.
		//
		if (o->next() != page->end()) {
			auto& free_list  = page->is_exec ? ex_free_list : free_lists[size_class_of(o->num_chunks)];
			o->gc_type      = type_gc_free;
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
		L->globals->gc_tick(s);

		// Strings.
		//
		((header*) L->empty_string)->gc_tick(s);
		((header*) L->strset)->gc_tick(s);
	}

	void state::close(vm* L) {
		// Clear stack and globals.
		//
		L->stack_top = L->stack;
		fill_none(L->globals->begin(), L->globals->realsize() * 2);

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
		// Reset GC tick.
		//
		ticks = gc_interval;
		debt  = 0;

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
		for_each([&](page* it, bool ex) {
			if (it->alive_objects != it->num_objects) {
				it->for_each([&](header* obj) {
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
		uint32_t length   = (uint32_t) (chunk_ceil(n + sizeof(header)) >> chunk_shift);
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