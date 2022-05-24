#pragma once
#include <util/common.hpp>
#include <lang/types.hpp>

namespace lightning::core {
	// GC types:
	//
	static constexpr size_t minimum_gc_allocation = 2 * 1024 * 1024;
	struct gc_page {
		gc_page* prev        = nullptr;
		gc_page* next        = nullptr;
		uint32_t num_pages   = 0;
		uint32_t num_objects = 0;
		uint32_t free_qwords = 0;

		// Default construction for head.
		//
		constexpr gc_page() : prev(this), next(this) {}

		// Constructed with number of pages.
		//
		constexpr gc_page(size_t num_pages) : num_pages((uint32_t) num_pages), free_qwords(uint32_t(((num_pages << 12) - sizeof(gc_page)) >> 3)) {}

		// Allocation helper.
		//
		template<typename T>
		bool check(size_t extra_length = 0) const {
			uint32_t length = uint32_t((extra_length + sizeof(T) + 7) >> 3);
			return free_qwords >= length;
		}
		template<typename T, typename... Tx>
		T* create(size_t extra_length = 0, Tx&&... args) {
			uint32_t length = uint32_t((extra_length + sizeof(T) + 7) >> 3);
			if (free_qwords >= length) {
				uint64_t* base = ((uint64_t*) this) + (size_t(num_pages) << (12 - 3));
				base -= free_qwords;
				free_qwords -= length;

				T* result = new (base) T(std::forward<Tx>(args)...);
				result->size_in_qwords = length;
				return result;
			}
			return nullptr;
		}
	};
	struct gc_header {
		uint32_t size_in_qwords : 30 = 0;
		uint32_t lock : 1            = 0;
		uint32_t traversable : 1     = 0;
		uint32_t reserved;

		// Object size helper.
		//
		size_t object_bytes() const { return size_t(size_in_qwords - 1) * 8; }

		// Next helper.
		//
		gc_header* next() { return (gc_header*) (((uint64_t*) this) + size_in_qwords); }

		// Constructed with whether or not it is traversable, gc_page fills the size.
		//
		constexpr gc_header(bool traversable) : traversable(traversable) {}
	};
	static_assert(sizeof(gc_header) == 8, "Invalid GC header size.");

	template<typename T, bool Traversable = false>
	struct gc_tag : gc_header {
		// Default constructed, no copy.
		//
		constexpr gc_tag() : gc_header(Traversable) {}

		// No copy.
		//
		gc_tag(const gc_tag&) = delete;
		gc_tag& operator=(const gc_tag&) = delete;
	};
	template<typename T> using gc_node = gc_tag<T, true>;
	template<typename T> using gc_leaf = gc_tag<T, false>;
};