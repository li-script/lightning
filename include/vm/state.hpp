#pragma once
#include <util/format.hpp>
#include <util/llist.hpp>
#include <util/platform.hpp>
#include <vm/gc.hpp>

namespace lightning::core {
	struct vm;

	// Panic function, should not return.
	//
	using fn_panic = void (*)(vm* L, const char* msg);
	static void default_panic [[noreturn]] (vm* L, const char* msg) { util::abort("[Lightning Panic] %s\n", msg); }

	// Forward for string set.
	//
	struct string_set;
	string_set* create_string_set(vm* L);

	// VM state.
	//
	struct vm : gc_leaf<vm> {
		static vm* create(fn_alloc alloc = &platform::page_alloc, size_t context_space = 0);

		// VM state.
		//
		fn_alloc    alloc_fn     = nullptr;         // Page allocator.
		gc_page     gc_page_head = {};              // GC linked-list head.
		fn_panic    panic_fn     = &default_panic;  // Panic function.
		string_set* str_intern   = nullptr;         // String interning state
		table*      globals      = nullptr;         // Globals.
		uint64_t    prng_seed    = 0;               // PRNG seed.
		any*        stack        = nullptr;         // Stack base.
		uint32_t    stack_top    = 0;               // Top of the stack.
		uint32_t    stack_len    = 0;               // Maximum length of the stack.

		// Closes the VM state.
		//
		void close();

		// Doubles the stack range.
		//
		LI_COLD void grow_stack() {
			stack = (any*) realloc(stack, (stack_len + 1) * 2 * sizeof(any));
			memset(&stack[stack_len], 0xFF, stack_len * sizeof(any));
			stack_len *= 2;
		}

		// Pushes to or pops from the stack.
		//
		void push_stack(any x) {
			if (stack_top == stack_len) [[unlikely]] {
				grow_stack();
			}
			stack[stack_top++] = x;
		}
		any pop_stack() {
			if (stack_top == 0) {
				return any{};
			} else {
				return stack[--stack_top];
			}
		}


		// Gets next random.
		//
		uint64_t random() { 
			prng_seed = (6364136223846793005 * prng_seed + 1442695040888963407);
			return prng_seed;
		}

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