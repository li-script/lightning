#pragma once
#include <util/format.hpp>
#include <util/llist.hpp>
#include <vm/gc.hpp>

namespace lightning::core {
	struct vm;

	// Panic function, should not return.
	//
	using fn_panic = void (*)(vm* L, const char* msg);
	static void default_panic [[noreturn]] (vm* L, const char* msg) { util::abort("[Lightning Panic] %s\n", msg); }

	// Page allocator:
	// - f(nullptr, N, ?) =     allocation
	// - f(ptr,     N, false) = free
	using fn_alloc = void* (*) (vm* L, void* pointer, size_t page_count, bool executable);
	extern void* default_allocator(vm* L, void* pointer, size_t page_count, bool executable);

	// VM state.
	//
	struct vm : gc_leaf<vm> {
		static vm* create(fn_alloc a = &default_allocator, size_t context_space = 0);

		// Allocator and GC page head.
		//
		fn_alloc alloc_fn     = nullptr;
		gc_page  gc_page_head = {};

		// Panic function.
		//
		fn_panic panic_fn = &default_panic;

		// String interning state.
		//
		table* str_intern = nullptr;

		// TODO:
		//  prng state
		//  jit state

		// Gets user context.
		//
		void* context() { return this + 1; }

		// Wrappers around the configurable function pointers.
		//
		void panic [[noreturn]] (const char* msg) {
			panic_fn(this, msg);
			assume_unreachable();
		}
		gc_page* add_page(size_t min_size, bool exec) {
			min_size     = std::max(min_size, minimum_gc_allocation);
			min_size     = (min_size + 0xFFF) >> 12;
			void* alloc = alloc_fn(this, nullptr, min_size, exec);
			if (!alloc)
				panic(util::fmt("failed to allocate %llu pages, out of memory.", min_size).c_str());
			auto* result = new (alloc) gc_page(min_size);
			util::link_before(&gc_page_head, result);
			return result;
		}
		void free_page(gc_page* gc) {
			util::unlink(gc);
			alloc_fn(this, gc, gc->num_pages, false);
		}

		// Allocation helper.
		//
		template<typename T, typename... Tx>
		T* alloc(size_t extra_length = 0, Tx&&... args) {
			auto* page = gc_page_head.prev;
			if (!page->check<T>(extra_length)) {
				page = add_page(sizeof(T) + extra_length, false);
			}
			return page->create<T, Tx...>(extra_length, std::forward<Tx>(args)...);
		}
	};
}