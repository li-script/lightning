#pragma once
#include <algorithm>
#include <optional>
#include <span>
#include <util/format.hpp>
#include <util/llist.hpp>
#include <util/platform.hpp>
#include <vm/gc.hpp>
#include <lib/fs.hpp>
#include <util/fastlock.hpp>

#ifndef LI_STACK_SIZE
	#define LI_STACK_SIZE (4 * 1024 * 1024)
#endif
#ifndef LI_SAFE_STACK
	#define LI_SAFE_STACK 0
#endif

namespace li {
	struct vm;

	// Native callback, at most one result should be pushed on stack, if returns false, signals exception.
	//
	using nfunc_t = any_t(*LI_CC)(vm* L, any* args, slot_t n);

	// nfunc_t for virtual functions.
	//  Caller must push all arguments in reverse order, the self argument or nil, the function itself and the caller information.
	//
	any_t LI_CC vm_invoke(vm* L, any* args, slot_t n_args);

	// Panic function, should not return.
	//
	using fn_panic = void (*)(vm* L, const char* msg);
	inline static void default_panic [[noreturn]] (vm* L, const char* msg) { util::abort("li panic: %s", msg); }

	// Forward for string and type sets.
	//
	struct string_set;
	struct type_set;
	void strset_init(vm* L);
	void typeset_init(vm* L);
	void strset_sweep(vm* L, gc::stage_context s);
	void typeset_sweep(vm* L, gc::stage_context s);
	vclass* typeset_fetch(vm* L, type id);

	// Call frame as a linked list of caller records.
	//
	static constexpr msize_t  MAX_ARGS     = 32;
	static constexpr slot_t   FRAME_SELF   = -3;  // specials relative to local 0
	static constexpr slot_t   FRAME_TARGET = -2;
	static constexpr slot_t   FRAME_CALLER = -1;
	static constexpr slot_t   FRAME_SIZE   = 3;
	static constexpr uint64_t FRAME_C_FLAG = (1ll << 17);
	static constexpr slot_t   STACK_LENGTH = LI_STACK_SIZE / sizeof(any);
	static constexpr slot_t   BC_MAX_IP    = FRAME_C_FLAG - 1;
	static_assert(STACK_LENGTH <= util::fill_bits(23), "Stack configured too large.");

	struct call_frame {
		// [locals of caller]
		// argn>
		// ..
		// arg0
		// self
		// fn <=> retval
		// [call_frame for previous function as any [will be number typed]]
		// [locals of this func]
		//
		uint64_t caller_pc : 23 = 0;  // Instruction pointer after the call.
		uint64_t stack_pos : 23 = 0;  // Stack position of frame (@local0).
		uint64_t rsvd : 18      = 0;

		inline constexpr bool multiplexed_by_c() const { return caller_pc & FRAME_C_FLAG; }
	};
	static_assert(sizeof(call_frame) == sizeof(any), "Invalid call frame size.");

	// VM state.
	//
	struct vm : gc::leaf<vm> {
		static vm*              create(fn_alloc alloc = &platform::page_alloc, void* allocu = nullptr);

		// VM state.
		//
		gc::state      gc           = {};                // Garbage collector.
		type_set*      typeset      = nullptr;           // Usertype database.
		string_set*    strset       = nullptr;           // String interning state
		string*        empty_string = nullptr;           // Constant "".
		table*         modules      = nullptr;           // Module table.
		table*         repl_scope   = nullptr;           // Repl scope.
		uint64_t       prng_seed    = platform::srng();  // PRNG seed.
		util::fastlock lock         = {};                // Lock protecting the VM.

		// Configuration.
		//
		lib::fs::fn_import import_fn       = &lib::fs::default_import;  // Import function.
		fn_panic           panic_fn        = &default_panic;            // Panic function.
		uint32_t           jit_all : 1     = false;                     // JITs every parsed function.
		uint32_t           jit_verbose : 1 = false;                     // JIT compilation prints debug information.

		// Stack.
		//
		any        last_ex        = {};       // Last exception thrown.
		call_frame last_vm_caller = {};       // Last valid VM frame that called into C.
		any*       stack_top      = nullptr;  // Top of the stack.
		any        stack[];                   // Stack base.

		// Closes the VM state.
		//
		void close();

		// Pushes to or pops from the stack.
		//
		LI_INLINE void push_stack(any x) {
#if LI_SAFE_STACK
			if (stack_top >= (stack + STACK_LENGTH)) [[unlikely]] {
				panic("stack too large.");
			}
#endif
			*stack_top++ = x;
		}
		LI_INLINE any* alloc_stack(slot_t n) {
			any* s = stack_top;
			stack_top += n;
#if LI_SAFE_STACK
			if (stack_top >= (stack + STACK_LENGTH)) [[unlikely]] {
				panic("stack too large.");
			}
#endif
			return s;
		}
		LI_INLINE void pop_stack_n(slot_t n) {
#if LI_SAFE_STACK
			n = std::min<slot_t>(n, stack_top - stack);
#endif
			stack_top -= n;
		}
		LI_INLINE any peek_stack() { return stack_top[-1]; }
		LI_INLINE any pop_stack() {
#if LI_SAFE_STACK
			if (stack_top == stack) {
				return nil;
			}
#endif
			return *--stack_top;
		}

		// Error/OK helper.
		//
		template<typename... Tx>
		LI_COLD any_t error(const char* fmt, Tx... args);

		any_t error(any result = nil) {
			last_ex = result;
			return exception_marker;
		}
		any_t ok(any result = nil) {
			return result;
		}

		// Gets next random.
		//
		uint64_t random() {
			prng_seed = (6364136223846793005 * prng_seed + 1442695040888963407);
			return prng_seed;
		}

		// Wrappers around the configurable function pointers.
		//
		void panic [[noreturn]] (const char* msg) {
			panic_fn(this, msg);
			assume_unreachable();
		}

		// Simple call wrapping vm_invoke for user-invocation that pops all arguments and the function/self from ToS.
		//
		LI_INLINE any call(slot_t n_args, any fn, any self = nil) {
			any* stack_reset_pos = stack_top - n_args;
			push_stack(self);
			push_stack(fn);
			call_frame cf{.caller_pc = msize_t(last_vm_caller.caller_pc | FRAME_C_FLAG), .stack_pos = last_vm_caller.stack_pos};
			push_stack(any_t{li::bit_cast<uint64_t>(cf)});
			any result = vm_invoke(this, &this->stack_top[-1 - FRAME_SIZE], n_args);
			stack_top = stack_reset_pos;
			return result;
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

		// Duplication helper.
		//
		template<typename T>
		T* duplicate(const T* gc, size_t extra_size = 0) {
			if (!gc)
				return nullptr;
			size_t obj_len = gc->object_bytes();
			T*     result  = alloc<T>(extra_size + obj_len - (sizeof(T) - sizeof(gc::header)));
			memcpy((void*) std::next((gc::header*) result), (void*) std::next((gc::header*) gc), obj_len);
			return result;
		}
	};

	// RAII helper to acquire VM lock if user plans using it in a multi-threaded fashion.
	//
	struct vm_thread_guard {
		std::unique_lock<util::fastlock> lock;
		vm_thread_guard() = default;
		vm_thread_guard(vm* L) : lock(L->lock) {}
	};

	// RAII helper to acquire VM state if user plans on calling any virtual functions or using stack trace info.
	//
	struct vm_stack_guard {
		vm* L;
		call_frame prev_frame;
		any*       prev_stack;
		vm_stack_guard(vm* L, any* a) : L(L) {
			if (&a[3] >= L->stack_top) {
				// Called from C.
				L = nullptr;
			} else {
				prev_frame        = L->last_vm_caller;
				L->last_vm_caller = li::bit_cast<call_frame>(a[3].value);
				prev_stack        = L->stack_top;
			}
		}
		~vm_stack_guard() {
			if (L) [[likely]] {
				LI_ASSERT(L->stack_top == prev_stack);
				L->last_vm_caller = prev_frame;
			}
		}

		// No copy.
		//
		vm_stack_guard(const vm_stack_guard&)      = delete;
		vm_stack_guard& operator=(const vm_stack_guard&) = delete;
	};
}