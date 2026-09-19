#pragma once
#include <algorithm>
#include <cstring>
#include <iterator>
#include <lib/fs.hpp>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <util/context.hpp>
#include <util/fastlock.hpp>
#include <util/format.hpp>
#include <util/llist.hpp>
#include <util/platform.hpp>
#include <vm/rc.hpp>
#include <vm/tier.hpp>

#ifndef LI_STACK_SIZE
	#define LI_STACK_SIZE (4 * 1024 * 1024)
#endif
#ifndef LI_SAFE_STACK
	#define LI_SAFE_STACK 0
#endif

namespace li {
	struct vm;
	struct coroutine_state;
	struct nfunc_info;

	void LI_CC              exception_location(vm* L, int32_t bytecode_pc);
	extern const nfunc_info exception_location_info;

	// Native arguments are borrowed for the duration of the call. The return value is owned.
	using nfunc_t = any_t(LI_CC*)(vm* L, any* args, slot_t n);

	any_t LI_CC vm_invoke(vm* L, any* args, slot_t n_args);

	// Resumes the interpreter in an existing owning invocation frame. The caller
	// supplies the actual argument count and bytecode offsets; locals already
	// occupy the VM stack and are neither allocated nor zero-initialized again.
	inline constexpr uint32_t no_interpreter_handler = UINT32_MAX;
	any_t LI_CC               vm_interpret_resume(vm* L, any* args, slot_t n_args, uint32_t bytecode_pc, uint32_t exception_handler_pc = no_interpreter_handler,
		 uint32_t cleanup_handler_pc = no_interpreter_handler);

	using fn_panic = void (*)(vm* L, const char* msg);
	inline static void default_panic [[noreturn]](vm*, const char* msg) { util::abort("li panic: %s", msg); }

	struct string_set;
	struct type_set;
	void    strset_init(vm* L);
	void    strset_remove(vm* L, string* value);
	void    typeset_init(vm* L);
	vclass* typeset_fetch(vm* L, type id);
	void    shutdown_module_records(vm* L);

	static constexpr msize_t  MAX_ARGS     = 32;
	static constexpr slot_t   FRAME_SELF   = -3;
	static constexpr slot_t   FRAME_TARGET = -2;
	static constexpr slot_t   FRAME_CALLER = -1;
	static constexpr slot_t   FRAME_SIZE   = 3;
	static constexpr uint64_t FRAME_C_FLAG = (1ll << 17);
	static constexpr slot_t   STACK_LENGTH = LI_STACK_SIZE / sizeof(any);
	static constexpr slot_t   BC_MAX_IP    = FRAME_C_FLAG - 1;
	static_assert(STACK_LENGTH <= util::fill_bits(23), "Stack configured too large.");

	struct call_frame {
		uint64_t caller_pc : 23 = 0;
		uint64_t stack_pos : 23 = 0;
		uint64_t rsvd : 18      = 0;

		inline constexpr bool multiplexed_by_c() const { return caller_pc & FRAME_C_FLAG; }
	};
	static_assert(sizeof(call_frame) == sizeof(any), "Invalid call frame size.");

	// The fields which follow the currently executing native stack. A coroutine switch
	// moves this bundle as one unit so stack-relative frame indices and exception state
	// always describe the same execution context.
	struct vm_execution_state {
		any*        stack                = nullptr;
		any*        stack_top            = nullptr;
		any*        stack_limit          = nullptr;
		any         last_ex              = {};
		array*      last_exception_trace = nullptr;
		call_frame  last_vm_caller       = {};
		const void* exception_handler    = nullptr;
		const void* cleanup_handler      = nullptr;
		uint32_t    cleanup_depth        = 0;
	};

	struct vm : gc::leaf<vm> {
		static vm* create(fn_alloc alloc = &platform::page_alloc, void* allocu = nullptr);

		gc::state      gc              = {};
		type_set*      typeset         = nullptr;
		string_set*    strset          = nullptr;
		string*        empty_string    = nullptr;
		table*         modules         = nullptr;
		table*         module_records  = nullptr;
		table*         repl_scope      = nullptr;
		vclass*        coroutine_class = nullptr;
		vclass*        iterator_class  = nullptr;
		vclass*        range_class     = nullptr;
		uint64_t       prng_seed       = platform::srng();
		util::fastlock lock            = {};

		lib::fs::fn_import import_fn = &lib::fs::default_import;
		fn_panic           panic_fn  = &default_panic;
#if LI_JIT
		tier::policy jit_policy = {tier::mode::automatic};
#else
		tier::policy jit_policy = {tier::mode::off};
#endif
		uint64_t jit_compile_ns       = 0;
		uint64_t generated_code_bytes = 0;
		uint64_t jit_compiled         = 0;
		uint64_t jit_spill_slots      = 0;
		uint64_t jit_guards           = 0;  // Type guards left in compiled code across all functions.
		bool     verbose_errors       = false;

		any                      last_ex              = {};
		array*                   last_exception_trace = nullptr;
		call_frame               last_vm_caller       = {};
		const void*              exception_handler    = nullptr;
		const void*              cleanup_handler      = nullptr;
		uint32_t                 cleanup_depth        = 0;
		any*                     stack                = nullptr;
		any*                     stack_top            = nullptr;
		any*                     stack_limit          = nullptr;
		coroutine_state*         current_coroutine    = nullptr;
		vm_execution_state       main_execution       = {};
		platform::native_context main_context         = {};
		uint32_t                 active_locks         = 0;
		any                      main_stack[];

		void close();

		// These queries are shared by the interpreter and JIT failure paths. They never
		// mutate coroutine state and never allocate. Native-stack required_bytes is the
		// incoming frame allocation in addition to the fixed native-call reserve.
		bool forced_unwind_pending() const noexcept;
		bool forced_unwind_requested() const noexcept;
		bool native_stack_headroom_available(std::size_t required_bytes = 0) const noexcept;
		bool vm_stack_headroom_available(std::size_t required_slots) const noexcept {
			return stack_top >= stack && stack_top <= stack_limit && required_slots <= std::size_t(stack_limit - stack_top);
		}

		LI_INLINE void push_stack(any value) {
#if LI_SAFE_STACK
			if (stack_top >= stack_limit) [[unlikely]]
				panic("stack too large.");
#endif
			rc::retain_frame(this, value);
			*stack_top++ = value;
		}

		LI_INLINE any* alloc_stack(slot_t count) {
			if (count < 0) [[unlikely]]
				panic("invalid stack allocation.");
			any* result = stack_top;
#if LI_SAFE_STACK
			if (count > (stack_limit - stack_top)) [[unlikely]]
				panic("stack too large.");
#endif
			fill_nil(result, size_t(count));
			stack_top += count;
			return result;
		}

		void truncate_stack(any* end);

		LI_INLINE void pop_stack_n(slot_t count) {
#if LI_SAFE_STACK
			count = std::clamp<slot_t>(count, 0, stack_top - stack);
#endif
			truncate_stack(stack_top - count);
		}

		LI_INLINE any peek_stack() { return stack_top[-1]; }

		LI_INLINE any pop_stack() {
#if LI_SAFE_STACK
			if (stack_top == stack)
				return nil;
#endif
			any* slot   = --stack_top;
			any  result = *slot;
			*slot       = nil;
			return result;
		}

		template<typename... Tx>
		LI_COLD any_t error(const char* fmt, Tx... args);
		LI_COLD any_t vm_stack_exhausted();

		// Publishes an exception payload and captures its live script/native call
		// chain. Passing the same payload again is a rethrow and preserves the
		// original trace.
		any_t       set_exception(any result, function* origin = nullptr, uint32_t origin_pc = UINT32_MAX, any* origin_local0 = nullptr);
		any_t       adopt_exception(any result, function* origin = nullptr, uint32_t origin_pc = UINT32_MAX, any* origin_local0 = nullptr);
		void        capture_exception_trace(function* origin, uint32_t origin_pc, any* origin_local0);
		array*      copy_exception_trace(any error);
		std::string format_exception_trace();

		// Drops the pending exception and its trace together; the two are one root.
		void clear_exception();

		any_t error(any result = nil) {
			set_exception(result);
			return exception_marker;
		}

		any_t ok(any result = nil) {
			if (!rc::try_retain(this, result))
				return exception_marker;
			return result;
		}

		any_t take(any result) { return rc::check_store(this, result) ? result : exception_marker; }

		uint64_t random() {
			prng_seed = (6364136223846793005 * prng_seed + 1442695040888963407);
			return prng_seed;
		}

		void panic [[noreturn]](const char* msg) {
			panic_fn(this, msg);
			assume_unreachable();
		}

		LI_INLINE any call(slot_t n_args, any fn, any self = nil) {
			if (n_args < 0 || n_args > (stack_top - stack)) [[unlikely]]
				panic("invalid call argument count.");
			any* stack_reset_pos = stack_top - n_args;
			if (!vm_stack_headroom_available(FRAME_SIZE)) [[unlikely]] {
				truncate_stack(stack_reset_pos);
				return vm_stack_exhausted();
			}
			push_stack(self);
			push_stack(fn);
			call_frame frame{
				 .caller_pc = msize_t(last_vm_caller.caller_pc | FRAME_C_FLAG),
				 .stack_pos = last_vm_caller.stack_pos,
			};
			push_stack(any_t{li::bit_cast<uint64_t>(frame)});
			any result = vm_invoke(this, &stack_top[-1 - FRAME_SIZE], n_args);
			truncate_stack(stack_reset_pos);
			return result;
		}

		template<typename T, typename... Tx>
		T* alloc(size_t extra_length = 0, Tx&&... args) {
			T* result = gc.create<T, Tx...>(this, extra_length, std::forward<Tx>(args)...);
			if (!result) [[unlikely]]
				panic("out of memory");
			return result;
		}

		template<typename T>
		T* duplicate(const T* value, size_t extra_size = 0) {
			if (!value)
				return nullptr;
			size_t           object_length = value->object_bytes();
			constexpr size_t fixed_payload = sizeof(T) - sizeof(gc::header);
			if (object_length < fixed_payload || extra_size > (std::numeric_limits<size_t>::max() - (object_length - fixed_payload)))
				panic("object duplication size overflow");
			T* result = alloc<T>(extra_size + object_length - fixed_payload);
			std::memcpy(static_cast<void*>(std::next(static_cast<gc::header*>(result))),
				 static_cast<const void*>(std::next(static_cast<const gc::header*>(value))), object_length);
			return result;
		}
	};

	struct vm_thread_guard {
		std::unique_lock<util::fastlock> lock;
		vm_thread_guard() = default;
		vm_thread_guard(vm* L) : lock(L->lock) {}

		vm_thread_guard(const vm_thread_guard&)            = delete;
		vm_thread_guard& operator=(const vm_thread_guard&) = delete;
	};

	struct vm_stack_guard {
		vm*        L;
		call_frame prev_frame;
		any*       prev_stack;

		vm_stack_guard(vm* L, any* args) : L(L) {
			if (&args[3] >= L->stack_top) {
				L = nullptr;
			} else {
				prev_frame        = L->last_vm_caller;
				L->last_vm_caller = li::bit_cast<call_frame>(args[3].value);
				prev_stack        = L->stack_top;
			}
		}

		~vm_stack_guard() {
			if (L) [[likely]] {
				LI_ASSERT(L->stack_top == prev_stack);
				L->last_vm_caller = prev_frame;
			}
		}

		vm_stack_guard(const vm_stack_guard&)            = delete;
		vm_stack_guard& operator=(const vm_stack_guard&) = delete;
	};
}
