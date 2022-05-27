#pragma once
#include <util/common.hpp>
#include <util/llist.hpp>
#include <util/platform.hpp>
#include <util/format.hpp>
#include <lang/types.hpp>

namespace li {
	struct vm;
};
namespace li::gc {
	struct stage_context {
		uint32_t next_stage;
	};
	struct page;

	static constexpr size_t minimum_allocation       = 512 * 1024;

	// GC header for all types.
	//
	struct header {
		uint32_t size_in_qwords : 31 = 0;
		uint32_t traversable : 1     = 0;
		uint32_t stage : 2           = 0;
		uint32_t free : 1            = 0;
		uint32_t page_offset : 24    = 0;  // in page units.

		// Object size helper.
		// - Size ignoring the header.
		size_t object_bytes() const { return total_bytes() - sizeof(header); }
		// - Size with the header.
		size_t total_bytes() const { return size_t(size_in_qwords) * 8; }

		// Page helper.
		//
		page* get_page() const {
			uintptr_t pfn = (uintptr_t(this) >> 12) - page_offset;
			return (page*) (pfn << 12);
		}

		// Next helper.
		//
		header* next() { return (header*) (((uint64_t*) this) + size_in_qwords); }

		// Internals.
		//
		void               gc_init(page* p, vm* L, uint32_t qlen, bool traversable);
		bool               gc_tick(stage_context s, bool weak = false);
		virtual void       gc_traverse(stage_context s) = 0;
		virtual value_type gc_identify() const         = 0;
		void               gc_free();

		// Virtual destructor.
		//
		virtual ~header() = default;
	};
	static_assert(sizeof(header) == 16, "Invalid GC header size.");

	template<typename T, bool Traversable = false>
	struct tag : header {
		static constexpr bool gc_traversable = Traversable;

		// Returns the extra-space associated.
		//
		size_t extra_bytes() const { return total_bytes() - sizeof(T); }

		// No copy, default constructed.
		//
		constexpr tag()            = default;
		tag(const tag&)            = delete;
		tag& operator=(const tag&) = delete;
	};
	template<typename T, value_type V = value_type::type_none>
	struct leaf : tag<T, false> {
		void       gc_traverse(stage_context s) override {}
		value_type gc_identify() const override { return V; }
	};
	template<typename T, value_type V = value_type::type_none>
	struct node : tag<T, true> {
		value_type gc_identify() const override { return V; }
	};

	// GC page.
	//
	struct page {
		page*    prev           = nullptr;
		page*    next           = nullptr;
		uint32_t num_pages      = 0;
		uint32_t num_objects    = 0;
		uint32_t free_qwords    = 0;
		uint32_t alive_objects  = 0;

		// Default construction for head.
		//
		constexpr page() : prev(this), next(this) {}

		// Constructed with number of pages.
		//
		constexpr page(size_t num_pages) : num_pages((uint32_t) num_pages), free_qwords(uint32_t(((num_pages << 12) - sizeof(page)) >> 3)) {}

		// Object enumeration.
		//
		header* begin() { return (header*) (((uint64_t*) this) + ((sizeof(page) + 7) / 8)); }
		void* end() {
			uint64_t* base = ((uint64_t*) this) + (size_t(num_pages) << (12 - 3));
			return base - free_qwords;
		}
		template<typename F>
		header* for_each(F&& fn) {
			header* it    = begin();
			void*   limit = end();
			do {
				header* next = it->next();
				if (fn(it))
					return it;
				it = next;
			} while (it < end());
			return nullptr;
		}

		// Allocates an uninitialized chunk, caller must have checked for space.
		//
		void* allocate_uninit(uint32_t qword_length) {
			LI_ASSERT(free_qwords >= qword_length);

			uint64_t* base = ((uint64_t*) this) + (size_t(num_pages) << (12 - 3));
			base -= free_qwords;
			free_qwords -= qword_length;
			num_objects++;
			return base;
		}
	};

	// GC state.
	//
	struct state {
		fn_alloc alloc_fn     = nullptr;  // Page allocator.
		void*    alloc_ctx    = nullptr;  //
		page*    initial_page = nullptr;  // Initial page entry.
		size_t   gc_debt      = 0;        // Allocations made since last GC sweep.
		size_t   ticks        = 0;        // Tick counter.


		// Page enumerator.
		//
		template<typename F>
		page* for_each(F&& fn) {
			page* it = initial_page;
			do {
				page* next = it->next;
				if (fn(it))
					return it;
				it = next;
			} while (it != initial_page);
			return nullptr;
		}

		// Deletes all memory.
		//
		void close() {
			auto* alloc = alloc_fn;
			void* actx  = alloc_ctx;
			auto* head  = initial_page;

			for (auto it = head->next; it != head; it = it->next)
				alloc(actx, it, it->num_pages, false);
			alloc(actx, head, head->num_pages, false);
			alloc(actx, actx, 0, false);
		}

		// Tick.
		//
		void collect(vm* L);
		void tick(vm* L) {
			// TODO: Proper scheduling.
			if (++ticks > 128 || gc_debt > (1 * 1024 * 1024) / 8) [[unlikely]] {
				gc_debt = 0;
				collect(L);
			}
		}

		// Allocates an uninitialized chunk.
		//
		std::pair<page*, void*> allocate_uninit(vm* L, uint32_t qword_length) {
			auto* page = initial_page->prev;
			if (page->free_qwords < qword_length) {
				page = add_page(L, size_t(qword_length) * 8, false);
				if (!page)
					return {nullptr, nullptr};
			}
			gc_debt += qword_length;
			return {page, page->allocate_uninit(qword_length)};
		}

		// Allocates and creates an object.
		//
		template<typename T, typename... Tx>
		T* create(vm* L, size_t extra_length = 0, Tx&&... args) {
			uint32_t length   = uint32_t((extra_length + sizeof(T) + 7) >> 3);
			auto [page, base] = allocate_uninit(L, length);
			T* result         = new (base) T(std::forward<Tx>(args)...);
			result->gc_init(page, L, length, T::gc_traversable);
			return result;
		}

		// Page list management.
		//
		page* add_page(vm* L, size_t min_size, bool exec) {
			min_size    = std::max(min_size, minimum_allocation);
			min_size    = (min_size + 0xFFF) >> 12;
			void* alloc = alloc_fn(alloc_ctx, nullptr, min_size, exec);
			if (!alloc)
				return nullptr;
			auto* result = new (alloc) page(min_size);
			util::link_before(initial_page, result);
			return result;
		}
	};
};