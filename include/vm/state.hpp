#pragma once
#include <util/format.hpp>
#include <util/llist.hpp>
#include <util/platform.hpp>
#include <vm/gc.hpp>
#include <algorithm>

namespace li {
	struct vm;

	// Panic function, should not return.
	//
	using fn_panic = void (*)(vm* L, const char* msg);
	static void default_panic [[noreturn]] (vm* L, const char* msg) { util::abort("[Lightning Panic] %s\n", msg); }

	// Forward for string set.
	//
	struct string_set;
	void init_string_intern(vm* L);
	void traverse_string_set(vm* L, gc::sweep_state& s);

	// VM state.
	//
	struct vm : gc::leaf<vm> {
		static vm* create(fn_alloc alloc = &platform::page_alloc, void* allocu = nullptr, size_t context_space = 0);

		// VM state.
		//
		gc::state   gc           = {};              // Garbage collector.
		fn_panic    panic_fn     = &default_panic;  // Panic function.
		string_set* str_intern   = nullptr;         // String interning state
		string*     empty_string = nullptr;         // Constant "".
		table*      globals      = nullptr;         // Globals.
		uint64_t    prng_seed    = 0;               // PRNG seed.
		any*        stack        = nullptr;         // Stack base.
		uint32_t    stack_top    = 0;               // Top of the stack.
		uint32_t    stack_len    = 0;               // Maximum length of the stack.

		// Handles garbage collection.
		//
		void collect_garbage();

		// Closes the VM state.
		//
		void close();

		// Grows the stack range.
		//
		LI_COLD void grow_stack(uint32_t n = 0) {
			n     = std::max(n, stack_len);
			stack = (any*) realloc(stack, (stack_len + n) * sizeof(any));
			fill_none(&stack[stack_len], n);
			stack_len += n;
		}

		// Pushes to or pops from the stack.
		//
		void push_stack(any x) {
			if (stack_top == stack_len) [[unlikely]] {
				grow_stack(0);
			}
			stack[stack_top++] = x;
		}
		uint32_t alloc_stack(uint32_t n) {
			if ((stack_top + n) >= stack_len) [[unlikely]] {
				grow_stack(n);
			}
			uint32_t slot = stack_top;
			stack_top += n;
			return slot;
		}
		void pop_stack_n(uint32_t n) {
			stack_top = std::max(stack_top, n) - n;
		}
		any  peek_stack() { return stack[stack_top - 1]; }
		any  pop_stack() {
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

		// Calls the function at the first slot in callsite with the arguments following it
		// - Returns true on success and false if the VM throws an exception.
		// - In either case and will replace the function with a value representing
		//    either the exception or the result.
		//
		bool call(uint32_t callsite, uint32_t n_args);

		// Simple version of call() for user-invocation that pops all arguments and the function from ToS.
		//
		bool scall(uint32_t n_args) {
			uint32_t cs = stack_top - (n_args + 1);
			bool ok = call(cs, n_args);
			pop_stack_n(n_args);
			return ok;
		}

		// Allocation helper.
		//
		template<typename T, typename... Tx>
		T* alloc(size_t extra_length = 0, Tx&&... args) {
			T* result = gc.alloc<T, Tx...>(this, extra_length, std::forward<Tx>(args)...);
			if (!result) [[unlikely]]
				panic("out of memory");
			return result;
		}
	};
}