#pragma once
#include <algorithm>
#include <optional>
#include <span>
#include <util/format.hpp>
#include <util/llist.hpp>
#include <util/platform.hpp>
#include <vm/gc.hpp>

#ifndef LI_STACK_SIZE
	#define LI_STACK_SIZE (4 * 1024 * 1024)
#endif
#ifndef LI_SAFE_STACK
	#define LI_SAFE_STACK 0
#endif

namespace li {
	struct vm;

	// Panic function, should not return.
	//
	using fn_panic = void (*)(vm* L, const char* msg);
	static void default_panic [[noreturn]] (vm* L, const char* msg) { util::abort("[Lightning Panic] %s\n", msg); }

	// Forward for string set.
	//
	struct string_set;
	void strset_init(vm* L);
	void strset_sweep(vm* L, gc::stage_context s);

	// Call frame as a linked list of caller records.
	//
	static constexpr msize_t  MAX_ARGS     = 32;
	static constexpr slot_t   FRAME_SELF   = -3;  // specials relative to local 0
	static constexpr slot_t   FRAME_TARGET = -2;
	static constexpr slot_t   FRAME_RET    = -2;
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
		// [call_frame for previous function as opaque]
		// [locals of this func]
		//
		int64_t  stack_pos : 23 = 0;  // Stack position of frame (@local0).
		uint64_t caller_pc : 18 = 0;  // Instruction pointer after the call.
		int64_t  n_args : 6     = 0;
		uint64_t rsvd : 17      = 0;

		inline constexpr bool multiplexed_by_c() const { return caller_pc & FRAME_C_FLAG; }
	};
	static_assert(sizeof(call_frame) == sizeof(opaque), "Invalid call frame size.");

	// Builtin handler callback.
	//
	struct func_scope;
	struct expression;
	using fn_parse_builtin = std::optional<expression> (*)(func_scope& scope, std::string_view name, const expression& self, std::span<std::pair<expression, bool>> params);

	// VM state.
	//
	struct vm : gc::leaf<vm> {
		static vm*              create(fn_alloc alloc = &platform::page_alloc, void* allocu = nullptr);
		static constexpr size_t reserved_global_length = 32;

		// VM state.
		//
		gc::state        gc             = {};                // Garbage collector.
		fn_panic         panic_fn       = &default_panic;    // Panic function.
		string_set*      strset         = nullptr;           // String interning state
		string*          empty_string   = nullptr;           // Constant "".
		table*           globals        = nullptr;           // Globals.
		uint64_t         prng_seed      = platform::srng();  // PRNG seed.
		call_frame       last_vm_caller = {};                // Last valid VM frame that called into C.
		fn_parse_builtin builtin_parser = nullptr;           // Parser callback to emit bytecode for builtins.

		// VM stack.
		//
		any* stack_top = nullptr;  // Top of the stack.
		any  stack[];              // Stack base.

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
				return any{};
			}
#endif
			return *--stack_top;
		}

		// Error/OK helper.
		//
		template<typename... Tx>
		LI_COLD bool error(const char* fmt, Tx... args);

		bool error(any result = none) {
			stack_top[FRAME_RET] = result;
			return false;
		}
		bool ok(any result = none) {
			stack_top[FRAME_RET] = result;
			return true;
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

		// Caller must push all arguments in reverse order, then the self argument or none and the function itself.
		// - Caller frame takes the caller's base of stack and the PC receives the "return pointer".
		//
		bool call(slot_t n_args, slot_t caller_frame, msize_t caller_pc = FRAME_C_FLAG);

		// Simple version of call() for user-invocation that pops all arguments and the function/self from ToS.
		//
		bool scall(slot_t n_args, any fn, any self = none) {
			any* req_slot = stack_top - n_args;
			push_stack(self);
			push_stack(fn);
			bool ok   = call(n_args, last_vm_caller.stack_pos, msize_t(last_vm_caller.caller_pc | FRAME_C_FLAG));
			*req_slot = stack_top[FRAME_RET];
			stack_top = req_slot + 1;
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
}