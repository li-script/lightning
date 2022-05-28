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
	void traverse_string_set(vm* L, gc::stage_context s);

	// Pseudo-type for stack.
	//
	struct vm_stack : gc::leaf<vm_stack> {
		any list[];
	};

	// VM state.
	//
	struct vm : gc::leaf<vm> {
		static vm* create(fn_alloc alloc = &platform::page_alloc, void* allocu = nullptr, size_t context_space = 0);

		static constexpr size_t initial_stack_length   = 32;
		static constexpr size_t reserved_global_length = 32;

		// VM state.
		//
		gc::state   gc           = {};                // Garbage collector.
		fn_panic    panic_fn     = &default_panic;    // Panic function.
		string_set* str_intern   = nullptr;           // String interning state
		string*     empty_string = nullptr;           // Constant "".
		table*      globals      = nullptr;           // Globals.
		uint64_t    prng_seed    = platform::srng();  // PRNG seed.
		any*        stack        = nullptr;           // Stack base.
		uint32_t    stack_top    = 0;                 // Top of the stack.
		uint32_t    stack_len    = 0;                 // Maximum length of the stack.
		uint32_t    cframe       = UINT32_MAX;        // Last valid VM frame that called into C.

		// Closes the VM state.
		//
		void close();

		// Grows the stack range by n.
		//
		LI_COLD void grow_stack(uint32_t n = 0) {
			n     = std::max(n, stack_len);
			auto* new_stack = alloc<vm_stack>((stack_len + n) * sizeof(any));
			memcpy(new_stack->list, stack, stack_top * sizeof(any));
			stack_len = (uint32_t) (new_stack->extra_bytes() / sizeof(any));
			gc.free(std::prev((gc::header*) stack));
			stack = new_stack->list;
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

		// Error helper.
		//
		template<typename... Tx>
		LI_COLD bool error(const char* fmt, Tx... args);

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

		// Caller must push all arguments in reverse order, then the self argument or none and the function itself.
		// - Caller frame takes the caller's base of stack and the PC receives the "return pointer".
		//
		bool call(uint32_t n_args, uint32_t caller_frame, uint32_t caller_pc);

		// Simple version of call() for user-invocation that pops all arguments and the function/self from ToS.
		//
		bool scall(uint32_t n_args, any fn, any self = none) {
			uint32_t req_slot = stack_top - n_args;
			uint32_t ret_slot = stack_top + 1; // @fn
			push_stack(self);
			push_stack(fn);
			bool ok = call(n_args, cframe, UINT32_MAX);
			stack[req_slot] = stack[ret_slot];
			stack_top       = req_slot + 1;
			return ok;
		}

		// Allocation helper.
		//
		template<typename T, typename... Tx>
		T* alloc(size_t extra_length = 0, Tx&&... args) {
			T* result = gc.create<T, Tx...>(this, extra_length, std::forward<Tx>(args)...);
			if (!result) [[unlikely]]
				panic("out of memory");
			return result;
		}
	};
}