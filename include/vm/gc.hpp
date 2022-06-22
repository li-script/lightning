#pragma once
#include <array>
#include <vm/types.hpp>
#include <util/common.hpp>
#include <util/format.hpp>
#include <util/llist.hpp>
#include <util/platform.hpp>
#include <vector>

namespace li {
	struct vm;

	// Traversable types.
	//
	struct array;
	struct table;
	struct function;
	struct jfunction;
	struct function_proto;
	struct string_set;
	struct type_set;
	struct vclass;
};
namespace li::gc {
	struct header;

	// Context for header::gc_tick and traverse.
	//
	using stage_context = bool;
	void traverse(stage_context s, array* o);
	void traverse(stage_context s, table* o);
	void traverse(stage_context s, function* o);
	void traverse(stage_context s, function_proto* o);
	void traverse(stage_context s, object* o);
	void traverse(stage_context s, vclass* o);

	void destroy(vm* L, vclass* o);
	void destroy(vm* L, object* o);
	void destroy(vm* L, jfunction* o);

	// GC configuration.
	//
	static constexpr size_t   minimum_allocation    = 2 * 1024 * 1024;
	static constexpr size_t   minimum_allocation_ex = 512 * 1024;
	static constexpr size_t   chunk_shift           = 5;
	static constexpr size_t   chunk_size            = 1ull << chunk_shift;
	static constexpr uint32_t default_interval      = 1 << 10;
	static constexpr size_t   default_min_debt      = 4096 / chunk_size;
	static constexpr msize_t  default_max_debt      = minimum_allocation / (4 * chunk_size);
	static constexpr msize_t  num_size_classes      = 16;

	static constexpr size_t chunk_ceil(size_t v) { return (v + chunk_size - 1) & ~(chunk_size - 1); }
	static constexpr size_t chunk_floor(size_t v) { return v & ~(chunk_size - 1); }

	// GC header for all types.
	//
	struct page;

	struct header {
		uint32_t stage : 1        = 0;             // Stage.
		uint32_t is_static : 1    = 0;             // Allocated outside GC pages.
		uint32_t page_offset : 30 = 0;             // Offset to page in pages.
		uint32_t num_chunks       = 0;             // Number of chunks in this block.
		int32_t  type_id          = type_invalid;  // Type.
		uint32_t rsvd             = 0;

		// Free header helpers.
		//
		bool     is_free() const { return type_id == type_gc_free; }
		header*& ref_next_free() { return *(header**) (this + 1); }
		void     set_next_free(header* h) {
			LI_ASSERT(type_id == type_gc_free);
			ref_next_free() = h;
		}
		header* get_next_free() {
			LI_ASSERT(type_id == type_gc_free);
			return ref_next_free();
		}

		// Object size helper.
		// - Size ignoring the header.
		size_t object_bytes() const { return total_bytes() - sizeof(header); }
		// - Size with the header.
		size_t total_bytes() const { return size_t(num_chunks) << chunk_shift; }

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
		void gc_init(page* p, vm* L, msize_t qlen, value_type t);
		bool gc_tick(stage_context s, bool weak = false);
	};
	static_assert(sizeof(header) == 16, "Invalid GC header size.");
	static_assert((sizeof(header) + sizeof(uintptr_t)) <= chunk_size, "Invalid GC header size.");

	// Forward for any.
	//
	static value_type identify_value_type(const header* h) { return (value_type) std::max<int32_t>(0, h->type_id); }

	template<typename T>
	struct tag : header {
		// Returns the extra-space associated.
		//
		size_t extra_bytes() const { return total_bytes() - sizeof(T); }

		// Default constructed.
		//
		constexpr tag()            = default;
	};
	template<typename T, value_type V = type_gc_private>
	struct leaf : tag<T> {
		static constexpr value_type gc_type        = V;
		static constexpr bool       gc_executable  = false;
		static constexpr bool       gc_traversable = false;
	};
	template<typename T, value_type V = type_gc_private>
	struct node : tag<T> {
		static constexpr value_type gc_type        = V;
		static constexpr bool       gc_executable  = false;
		static constexpr bool       gc_traversable = true;
	};
	template<typename T, value_type V = type_gc_private>
	struct exec_leaf : tag<T> {
		static constexpr value_type gc_type        = V;
		static constexpr bool       gc_executable  = true;
		static constexpr bool       gc_traversable = false;
	};

	// GC page.
	//
	struct page {
		page*    prev             = this;
		page*    next             = this;
		uint32_t num_pages        = 0;
		uint32_t num_objects      = 0;
		uint32_t alive_objects    = 0;
		uint32_t next_chunk       = (uint32_t) chunk_ceil(sizeof(page));
		uint32_t is_permanent : 1 = false;
		uint32_t is_exec : 1      = false;

		// Constructed with number of pages.
		//
		constexpr page(size_t num_pages, bool exec) : num_pages((msize_t) num_pages), is_exec(exec) {}

		// Checks if the page has space to allocate N chunks at the end.
		//
		bool check_space(msize_t clen) const {
			msize_t capacity = num_pages << (12 - chunk_shift);
			return (capacity - next_chunk) > clen;
		}

		// Gets a pointer to a chunk by index.
		//
		void* get_chunk(msize_t idx) { return (void*) (uintptr_t(this) + chunk_size * idx); }

		// Object enumeration.
		//
		header* begin() { return (header*) get_chunk((msize_t) chunk_ceil(sizeof(page))); }
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
		header* alloc_arena(msize_t clen) {
			LI_ASSERT(check_space(clen));

			void* p = get_chunk(next_chunk);
			next_chunk += clen;
			num_objects++;
			return (header*) p;
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
		page* initial_page    = nullptr;
		page* initial_ex_page = nullptr;

		// Configuration.
		//
		msize_t interval = default_interval;  // Interval at which the GC ticks collect.
		msize_t max_debt = default_max_debt;  // Maximum debt after which GC collects immediately.
		msize_t min_debt = default_min_debt;  // Minimum debt before GC ticker starts counting.
		bool    greedy   = true;              // Holds onto pages even if there are no objects in them.

		// Scheduling details.
		//
		msize_t debt            = 0;                                // Allocations made since last GC sweep.
		int64_t ticks           = min_debt ? INT64_MAX : interval;  // Tick counter.
		msize_t collect_counter = 0;
		bool    suspend         = false;

		// Free lists.
		//
		std::array<header*, num_size_classes> free_lists   = {nullptr};
		header*                               ex_free_list = nullptr;

		// Page enumerator.
		//
		template<typename F>
		page* for_each_rw(F&& fn) {
			page* head = initial_page;
			page* it   = head;
			do {
				page* next = it->next;
				if (fn(it, false))
					return it;
				it = next;
			} while (it != head);
			return nullptr;
		}
		template<typename F>
		page* for_each_ex(F&& fn) {
			if (page* head = initial_ex_page) {
				page* it = head->next;
				while (true) {
					page* next = it->next;
					if (fn(it, true))
						return it;
					if (it == head)
						break;
					it = next;
				}
			}
			return nullptr;
		}
		template<typename F>
		page* for_each( F&& fn ) {
			if (page* p = for_each_rw(fn))
				return p;
			if (page* p = for_each_ex(fn))
				return p;
			return nullptr;
		}

		// Calls all destructors and deallocates all memory.
		//
		void close(vm* L);

		// Tick.
		//
		void collect(vm* L);
		void tick(vm* L) {
			--ticks;
			if (ticks <= 0) [[unlikely]] {
				collect(L);
			}
		}

		// Allocates an uninitialized chunk.
		//
		std::pair<page*, header*> allocate_uninit(vm* L, msize_t clen);
		std::pair<page*, header*> allocate_uninit_ex(vm* L, msize_t clen);

		// Immediately frees an object.
		//
		void free(vm* L, header* o, bool within_gc = false);

		// Allocates and creates an object.
		//
		template<typename T, typename... Tx>
		T* create(vm* L, size_t extra_length = 0, Tx&&... args) {
			msize_t length    = (msize_t) (chunk_ceil(extra_length + sizeof(T)) >> chunk_shift);
			auto [page, base] = T::gc_executable ? allocate_uninit_ex(L, length) : allocate_uninit(L, length);
			T* result         = new (base) T(std::forward<Tx>(args)...);
			result->gc_init(page, L, length, T::gc_type);
			return result;
		}

		// Page list management.
		//
		template<bool Exec>
		page* add_page(vm* L, size_t min_size) {
			min_size    = std::max(min_size, Exec ? minimum_allocation_ex : minimum_allocation);
			min_size    = (min_size + 0xFFF) >> 12;
			void* alloc = alloc_fn(alloc_ctx, nullptr, min_size, Exec);
			if (!alloc)
				return nullptr;
			auto* result = new (alloc) page(min_size, Exec);
			if constexpr (!Exec) {
				util::link_after(initial_page, result);
			} else {
				if (!initial_ex_page) {
					result->is_permanent = 1;  // Should never be free'd, just like the rw initial page.
					initial_ex_page = result;
				} else {
					util::link_after(initial_ex_page, result);
				}
			}
			return result;
		}
	};

	// Helper for creating constant GC values on heap.
	//
	template<typename T>
	static void make_non_gc(T* value, size_t extra_size = 0) {
		value->type_id     = T::gc_type;
		value->num_chunks  = (msize_t) (chunk_ceil(extra_size + sizeof(T)) >> chunk_shift);
		value->page_offset = 0x3FFFFFFF;
		value->stage       = 0;
		value->is_static   = true;
	}
	template<typename T>
	static T* make_non_gc(size_t extra_size = 0) {
		T* value = new (malloc(sizeof(T) + extra_size)) T();
		make_non_gc<T>(value, extra_size);
		return value;
	}

	// any[] helper.
	//
	LI_INLINE inline void traverse_n(stage_context s, any* begin, size_t count) {
		for (auto it = begin; it != (begin + count); it++) {
			if (it->is_gc())
				it->as_gc()->gc_tick(s);
		}
	}
};