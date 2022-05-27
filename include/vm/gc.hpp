#pragma once
#include <array>
#include <lang/types.hpp>
#include <util/common.hpp>
#include <util/format.hpp>
#include <util/llist.hpp>
#include <util/platform.hpp>

namespace li {
	struct vm;
};
namespace li::gc {
	// Context for header::gc_tick.
	//
	struct stage_context {
		uint32_t next_stage;
	};

	// GC configuration.
	//
	static constexpr size_t   minimum_allocation = 2 * 1024 * 1024;
	static constexpr size_t   chunk_shift        = 4;
	static constexpr size_t   chunk_size         = 1ull << chunk_shift;
	static constexpr uint32_t gc_interval        = 16384;
	static constexpr size_t   gc_min_debt        = 4096 / chunk_size;
	static constexpr size_t   gc_max_debt        = (1 * 1024 * 1024) / chunk_size;

	static constexpr size_t chunk_ceil(size_t v) { return (v + chunk_size - 1) & ~(chunk_size - 1); }
	static constexpr size_t chunk_floor(size_t v) { return v & ~(chunk_size - 1); }

	// Size classes.
	//
	static constexpr uint32_t size_classes[] = {
		32 >> chunk_shift,
		64 >> chunk_shift,
		128 >> chunk_shift,
		256 >> chunk_shift,
		1024 >> chunk_shift,
		2048 >> chunk_shift,
		UINT32_MAX
	};
	static constexpr size_t   size_class_of(uint32_t nchunks) {
		  for (size_t sc = 0; sc != std::size(size_classes); sc++) {
			if (nchunks <= size_classes[sc]) {
				return sc;
			}
		}
		assume_unreachable();
	}

	// GC header for all types.
	//
	struct page;

	struct free_header {
		uintptr_t valid : 1      = 0;  // Abusing the fact that VTable won't be misaligned.
#if UINTPTR_MAX == 0xFFFFFFFF
		intptr_t next_free : 31 = 0;  // Offset to next free block, 0 if last.
#else
		intptr_t next_free : 63 = 0;  // Offset to next free block, 0 if last.
#endif

		// Should match header:
		//
		uint32_t rsvd : 3         = 0;
		uint32_t num_chunks : 29  = 0;
		uint32_t page_offset : 24 = 0;  // in page units.

		// Next helper.
		//
		void         set_next(free_header* h) { next_free = h ? intptr_t(h) - intptr_t(this) : 0; }
		free_header* next() {
			free_header* h = (free_header*) (((uint8_t*) this) + next_free);
			return h != this ? h : nullptr;
		}
	};
	struct header {
		uint32_t traversable : 1  = 0;
		uint32_t stage : 2        = 0;
		uint32_t num_chunks : 29  = 0;
		uint32_t page_offset : 24 = 0;  // in page units.

		// Object size helper.
		// - Size ignoring the header.
		size_t object_bytes() const { return total_bytes() - sizeof(header); }
		// - Size with the header.
		size_t total_bytes() const { return size_t(num_chunks) << chunk_shift; }

		// Gets the free header.
		//
		free_header* get_free_header() const { return (free_header*) this; }
		bool         is_free() const { return get_free_header()->valid; }

		// Page helper.
		//
		page* get_page() const {
			uintptr_t pfn = (uintptr_t(this) >> 12) - page_offset;
			return (page*) (pfn << 12);
		}

		// Next helper.
		//
		header* next() { return (header*) (((uint8_t*) this) + total_bytes()); }

		// Internals.
		//
		void               gc_init(page* p, vm* L, uint32_t qlen, bool traversable);
		bool               gc_tick(stage_context s, bool weak = false);
		virtual void       gc_traverse(stage_context s) = 0;
		virtual value_type gc_identify() const          = 0;

		// Virtual destructor.
		//
		virtual ~header() = default;
	};
	static_assert(sizeof(header) == (8 + sizeof(uintptr_t)), "Invalid GC header size.");
	static_assert(sizeof(free_header) == sizeof(header), "Invalid GC header size.");
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
		page*    prev          = nullptr;
		page*    next          = nullptr;
		uint32_t num_pages     = 0;
		uint32_t num_objects   = 0;
		uint32_t alive_objects = 0;
		uint32_t next_chunk    = chunk_ceil(sizeof(page));

		// Default construction for head.
		//
		constexpr page() : prev(this), next(this) {}

		// Constructed with number of pages.
		//
		constexpr page(size_t num_pages) : num_pages((uint32_t) num_pages) {}

		// Checks if the page has space to allocate N chunks at the end.
		//
		bool check_space(uint32_t clen) const {
			uint32_t capacity = num_pages << (12 - chunk_shift);
			return (capacity - next_chunk) > clen;
		}

		// Gets a pointer to a chunk by index.
		//
		void* get_chunk(uint32_t idx) { return (void*) (uintptr_t(this) + chunk_size * idx); }

		// Object enumeration.
		//
		header* begin() { return (header*) get_chunk(chunk_ceil(sizeof(page))); }
		void*   end() { return get_chunk(next_chunk); }
		template<typename F>
		header* for_each(F&& fn) {
			header* it    = begin();
			void*   limit = end();
			while (it < limit) {
				header* next = it->next();
				if (fn(it))
					return it;
				it = next;
			}
			return nullptr;
		}

		// Allocates an uninitialized chunk, caller must have checked for space.
		//
		void* allocate_uninit(uint32_t clen) {
			LI_ASSERT(check_space(clen));

			void* p = get_chunk(next_chunk);
			next_chunk += clen;
			num_objects++;
			return p;
		}
	};

	// GC state.
	//
	struct state {
		// Page allocator.
		//
		fn_alloc alloc_fn  = nullptr;
		void*    alloc_ctx = nullptr;

		// Initial page entry.
		//
		page* initial_page = nullptr;

		// Scheduling details.
		//
		size_t  debt  = 0;            // Allocations made since last GC sweep.
		int32_t ticks = gc_interval;  // Tick counter.

		// Free lists.
		//
		std::array<free_header*, std::size(size_classes)> free_lists = {nullptr};

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

			for (auto it = head->next; it != head;) {
				auto p = std::exchange(it, it->next);
				alloc(actx, p, p->num_pages, false);
			}
			alloc(actx, head, head->num_pages, false);
			alloc(actx, actx, 0, false);
		}

		// Tick.
		//
		void collect(vm* L);
		void tick(vm* L) {
			// TODO: Proper scheduling.
			--ticks;
			if (debt > gc_min_debt) [[unlikely]] {
				if (ticks < 0 || debt > gc_max_debt) {
					collect(L);
				}
			}
		}

		// Allocates an uninitialized chunk.
		//
		std::pair<page*, void*> allocate_uninit(vm* L, uint32_t clen);

		// Immediately frees an object.
		//
		void free(header* o, bool internal = false);

		// Allocates and creates an object.
		//
		template<typename T, typename... Tx>
		T* create(vm* L, size_t extra_length = 0, Tx&&... args) {
			uint32_t length   = (uint32_t) (chunk_ceil(extra_length + sizeof(T)) >> chunk_shift);
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
			util::link_after(initial_page, result);
			return result;
		}
	};
};