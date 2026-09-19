#pragma once
#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <new>
#include <util/common.hpp>
#include <util/format.hpp>
#include <util/llist.hpp>
#include <util/platform.hpp>
#include <utility>
#include <vector>
#include <vm/types.hpp>

namespace li {
	struct vm;
	struct array;
	struct table;
	struct string;
	struct function;
	struct jfunction;
	struct function_proto;
	struct string_set;
	struct type_set;
	struct object;
	struct vclass;
	struct weak;
	struct typed_array;
}

namespace li::gc {
	struct header;

	void destroy(vm* L, array* value);
	void destroy(vm* L, table* value);
	void destroy(vm* L, string* value);
	void destroy(vm* L, function* value);
	void destroy(vm* L, function_proto* value);
	void destroy(vm* L, object* value);
	void destroy(vm* L, vclass* value);
	void destroy(vm* L, jfunction* value);
	void destroy(vm* L, weak* value);
	void destroy(vm* L, typed_array* value);
	void invalidate_weak(vm* L, header* value);
	void shutdown_weak(vm* L);

	static constexpr size_t   minimum_allocation  = 2 * 1024 * 1024;
	static constexpr size_t   chunk_shift         = 5;
	static constexpr size_t   chunk_size          = size_t{1} << chunk_shift;
	static constexpr msize_t  num_size_classes    = 16;
	static constexpr uint32_t destroying_refcount = std::numeric_limits<uint32_t>::max();
	static constexpr uint32_t maximum_refcount    = destroying_refcount - 1;

	static constexpr size_t chunk_ceil(size_t value) { return (value + chunk_size - 1) & ~(chunk_size - 1); }
	static constexpr size_t chunk_floor(size_t value) { return value & ~(chunk_size - 1); }

	struct page;

	struct header {
		uint32_t shared : 1       = 0;
		uint32_t is_static : 1    = 0;
		uint32_t page_offset : 30 = 0;
		uint32_t num_chunks       = 0;
		int32_t  type_id          = type_invalid;
		uint32_t refcount         = 0;

		bool     is_free() const { return type_id == type_gc_free; }
		header*& ref_next_free() { return *reinterpret_cast<header**>(this + 1); }
		void     set_next_free(header* value) {
			LI_ASSERT(type_id == type_gc_free);
			ref_next_free() = value;
		}
		header* get_next_free() {
			LI_ASSERT(type_id == type_gc_free);
			return ref_next_free();
		}

		size_t object_bytes() const { return total_bytes() - sizeof(header) - (shared ? sizeof(uint64_t) : 0); }
		size_t total_bytes() const { return size_t(num_chunks) << chunk_shift; }

		page* get_page() const {
			uintptr_t pfn = (uintptr_t(this) >> 12) - page_offset;
			return reinterpret_cast<page*>(pfn << 12);
		}

		header* next() { return reinterpret_cast<header*>(reinterpret_cast<uint8_t*>(this) + total_bytes()); }
		void    initialize(page* owner, msize_t chunks, value_type type);
	};
	static_assert(sizeof(header) == 16, "Invalid GC header size.");
	static_assert((sizeof(header) + sizeof(uintptr_t)) <= chunk_size, "Invalid GC header size.");

	static value_type identify_value_type(const header* value) { return static_cast<value_type>(std::max<int32_t>(0, value->type_id)); }

	template<typename T>
	struct tag : header {
		size_t extra_bytes() const { return object_bytes() - (sizeof(T) - sizeof(header)); }
		constexpr tag() = default;
	};

	template<typename T, value_type V = type_gc_private>
	struct leaf : tag<T> {
		static constexpr value_type gc_type = V;
	};

	template<typename T, value_type V = type_gc_private>
	struct node : tag<T> {
		static constexpr value_type gc_type = V;
	};

	struct page {
		page*    prev        = this;
		page*    next        = this;
		uint32_t num_pages   = 0;
		uint32_t num_objects = 0;
		uint32_t next_chunk  = uint32_t(chunk_ceil(sizeof(page)) >> chunk_shift);

		constexpr page(size_t count) : num_pages(msize_t(count)) {}

		bool check_space(msize_t chunks) const {
			msize_t capacity = num_pages << (12 - chunk_shift);
			return next_chunk <= capacity && chunks <= (capacity - next_chunk);
		}

		void* get_chunk(msize_t index) { return reinterpret_cast<void*>(uintptr_t(this) + chunk_size * index); }

		header* begin() { return static_cast<header*>(get_chunk(msize_t(chunk_ceil(sizeof(page)) >> chunk_shift))); }
		void*   end() { return get_chunk(next_chunk); }

		template<typename F>
		header* for_each(F&& fn) {
			header* current = begin();
			auto*   limit   = static_cast<uint8_t*>(end());
			while (reinterpret_cast<uint8_t*>(current) < limit) {
				header* next_value = current->next();
				if (fn(current))
					return current;
				current = next_value;
			}
			return nullptr;
		}

		header* alloc_arena(msize_t chunks) {
			LI_ASSERT(check_space(chunks));
			void* result = get_chunk(next_chunk);
			next_chunk += chunks;
			num_objects++;
			return static_cast<header*>(result);
		}
	};

	struct state {
		fn_alloc alloc_fn  = nullptr;
		void*    alloc_ctx = nullptr;

		page* initial_page = nullptr;

		std::array<header*, num_size_classes> free_lists = {nullptr};

		std::vector<header*> pending{};
		header*              finalizer_context = nullptr;
		void*                weak_registry     = nullptr;
		uint64_t             live_objects      = 0;
		uint64_t             allocations       = 0;
		bool                 destroying        = false;
		bool                 shutting_down     = false;
		bool                 closing           = false;
		bool                 shared_heap       = false;

		template<typename F>
		page* for_each(F&& fn) {
			page* head    = initial_page;
			page* current = head;
			do {
				page* next_value = current->next;
				if (fn(current, false))
					return current;
				current = next_value;
			} while (current != head);
			return nullptr;
		}

		void                      close(vm* L);
		std::pair<page*, header*> allocate_uninit(vm* L, msize_t chunks);
		void                      free_storage(header* value);

		template<typename T, typename... Tx>
		T* create(vm* L, size_t extra_length = 0, Tx&&... args) {
			if (closing || extra_length > (std::numeric_limits<size_t>::max() - sizeof(T)))
				return nullptr;
			size_t bytes = extra_length + sizeof(T);
			if (shared_heap) {
				if (bytes > std::numeric_limits<size_t>::max() - sizeof(uint64_t))
					return nullptr;
				bytes += sizeof(uint64_t);
			}
			if (bytes > (std::numeric_limits<size_t>::max() - (chunk_size - 1)))
				return nullptr;
			size_t chunk_count = chunk_ceil(bytes) >> chunk_shift;
			if (!chunk_count || chunk_count > std::numeric_limits<msize_t>::max())
				return nullptr;

			auto [owner, storage] = allocate_uninit(L, msize_t(chunk_count));
			if (!storage)
				return nullptr;

			T* result = new (storage) T(std::forward<Tx>(args)...);
			result->initialize(owner, msize_t(chunk_count), T::gc_type);
			result->shared = shared_heap;
			if (shared_heap) {
				auto* lock_storage = reinterpret_cast<uint8_t*>(result) + result->total_bytes() - sizeof(uint64_t);
				new (lock_storage) uint64_t(0);
			}
			live_objects++;
			allocations++;
			return result;
		}

		page* add_page(vm*, size_t object_bytes) {
			size_t page_overhead = chunk_ceil(sizeof(page));
			if (object_bytes > (std::numeric_limits<size_t>::max() - page_overhead))
				return nullptr;
			size_t bytes = std::max(minimum_allocation, object_bytes + page_overhead);
			if (bytes > (std::numeric_limits<size_t>::max() - 0xFFF))
				return nullptr;
			size_t           page_count         = (bytes + 0xFFF) >> 12;
			constexpr size_t maximum_page_count = size_t(std::numeric_limits<msize_t>::max()) >> (12 - chunk_shift);
			if (!page_count || page_count > maximum_page_count)
				return nullptr;

			void* allocation = alloc_fn(alloc_ctx, nullptr, page_count, false);
			if (!allocation)
				return nullptr;
			auto* result = new (allocation) page(page_count);
			util::link_after(initial_page, result);
			return result;
		}
	};

	template<typename T>
	static void make_non_gc(T* value, size_t extra_size = 0) {
		if (!value || extra_size > (std::numeric_limits<size_t>::max() - sizeof(T)))
			util::abort("invalid static GC allocation");
		size_t bytes = extra_size + sizeof(T);
		if (bytes > (std::numeric_limits<size_t>::max() - (chunk_size - 1)))
			util::abort("static GC allocation is too large");
		size_t chunks = chunk_ceil(bytes) >> chunk_shift;
		if (!chunks || chunks > std::numeric_limits<uint32_t>::max())
			util::abort("static GC allocation is too large");

		value->shared      = 0;
		value->is_static   = true;
		value->page_offset = 0x3FFFFFFF;
		value->num_chunks  = msize_t(chunks);
		value->type_id     = T::gc_type;
		value->refcount    = destroying_refcount;
	}

	template<typename T>
	static T* make_non_gc(size_t extra_size = 0) {
		if (extra_size > (std::numeric_limits<size_t>::max() - sizeof(T)))
			return nullptr;
		void* storage = std::malloc(sizeof(T) + extra_size);
		if (!storage)
			return nullptr;
		T* value = new (storage) T();
		make_non_gc(value, extra_size);
		return value;
	}
}
