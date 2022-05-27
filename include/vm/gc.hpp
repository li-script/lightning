#pragma once
#include <util/common.hpp>
#include <util/llist.hpp>
#include <util/platform.hpp>
#include <lang/types.hpp>

namespace li {
	struct vm;
};
namespace li::gc {
	struct sweep_state;

	// GC header for all types.
	//
	struct header {
		uint32_t size_in_qwords : 31 = 0;
		uint32_t traversable : 1     = 0;
		uint32_t stage : 1           = 0;

		// Object size helper.
		// - Size ignoring the header.
		size_t object_bytes() const { return total_bytes() - 16; }
		// - Size with the header.
		size_t total_bytes() const { return size_t(size_in_qwords) * 8; }

		// Next helper.
		//
		header* next() { return (header*) (((uint64_t*) this) + size_in_qwords); }

		// Internals.
		//
		void         gc_init(vm* L);
		bool         gc_tick(sweep_state& s, bool weak = false);
		virtual void gc_traverse(sweep_state& s) = 0;

		// Constructed with whether or not it is traversable, page fills the size.
		//
		constexpr header(bool traversable) : traversable(traversable) {}

		// Virtual destructor.
		//
		virtual ~header() = default;
	};
	static_assert(sizeof(header) == 16, "Invalid GC header size.");

	template<typename T, bool Traversable = false>
	struct tag : header {
		// Default constructed, no copy.
		//
		constexpr tag() : header(Traversable) {}

		// Returns the extra-space associated.
		//
		size_t extra_bytes() const { return total_bytes() - sizeof(T); }

		// No copy.
		//
		tag(const tag&) = delete;
		tag& operator=(const tag&) = delete;
	};
	template<typename T>
	struct leaf : tag<T, false> {
		void gc_traverse(sweep_state& s) override {}
	};
	template<typename T> using node = tag<T, true>;
	
	// GC page.
	//
	static constexpr size_t minimum_allocation = 2 * 1024 * 1024;
	struct page {
		page* prev        = nullptr;
		page* next        = nullptr;
		uint32_t num_pages   = 0;
		uint32_t num_objects = 0;
		uint32_t free_qwords = 0;

		// Default construction for head.
		//
		constexpr page() : prev(this), next(this) {}

		// Constructed with number of pages.
		//
		constexpr page(size_t num_pages) : num_pages((uint32_t) num_pages), free_qwords(uint32_t(((num_pages << 12) - sizeof(page)) >> 3)) {}

		// Allocation helper.
		//
		template<typename T>
		bool check(size_t extra_length = 0) const {
			uint32_t length = uint32_t((extra_length + sizeof(T) + 7) >> 3);
			return free_qwords >= length;
		}
		template<typename T, typename... Tx>
		T* create(vm* L, size_t extra_length = 0, Tx&&... args) {
			uint32_t length = uint32_t((extra_length + sizeof(T) + 7) >> 3);
			if (free_qwords >= length) {
				uint64_t* base = ((uint64_t*) this) + (size_t(num_pages) << (12 - 3));
				base -= free_qwords;
				free_qwords -= length;

				T* result = new (base) T(std::forward<Tx>(args)...);
				result->size_in_qwords = length;
				result->gc_init(L);
				num_objects++;
				return result;
			}
			return nullptr;
		}
	};

	// GC state.
	//
	struct state {
		fn_alloc alloc_fn       = nullptr;  // Page allocator.
		void*    alloc_ctx      = nullptr;  //
		page     page_list_head = {};       // Head of page list.
		bool     stage          = false;

		// Deletes all memory.
		//
		void close() {
			auto* alloc = alloc_fn;
			void* actx   = alloc_ctx;
			auto* head  = &page_list_head;

			for (auto it = page_list_head.prev; it != head;) {
				auto nit = it->prev;
				alloc(actx, it, it->num_pages, false);
				it = nit;
			}
			alloc(actx, actx, 0, false);
		}

		// Tick.
		//
		void collect(vm* L);
		void tick(vm* L) { collect(L); }

		// Allocation helper.
		//
		template<typename T, typename... Tx>
		T* alloc(vm* L, size_t extra_length = 0, Tx&&... args) {
			auto* page = page_list_head.prev;
			if (!page->check<T>(extra_length)) {
				page = add_page(L, sizeof(T) + extra_length, false);
				if (!page)
					return nullptr;
			}
			return page->create<T, Tx...>(L, extra_length, std::forward<Tx>(args)...);
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
			util::link_before(&page_list_head, result);
			return result;
		}
		void free_page(vm* L, page* gc) {
			util::unlink(gc);
			alloc_fn(alloc_ctx, gc, gc->num_pages, false);
		}
	};
};