#include <util/context.hpp>

#if LI_CONTEXT_SUPPORTED

	#include <exception>
	#include <utility>

	#if LI_ARCH_X86
		#include <xmmintrin.h>
	#endif

	#if LI_WINDOWS || LI_OSX
		#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
			#define LI_CONTEXT_SANITIZER_HOOKS 1
		#else
			#define LI_CONTEXT_SANITIZER_HOOKS 0
		#endif
		#define LI_CONTEXT_SANITIZER_HOOKS_WEAK 0
		#define LI_CONTEXT_SANITIZER_WEAK
	#else
		#define LI_CONTEXT_SANITIZER_HOOKS      1
		#define LI_CONTEXT_SANITIZER_HOOKS_WEAK 1
		#define LI_CONTEXT_SANITIZER_WEAK       __attribute__((weak))
	#endif

	#if LI_CONTEXT_SANITIZER_HOOKS
extern "C" LI_CONTEXT_SANITIZER_WEAK void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* stack_bottom, std::size_t stack_size) noexcept;
extern "C" LI_CONTEXT_SANITIZER_WEAK void __sanitizer_finish_switch_fiber(
	 void* fake_stack_save, const void** stack_bottom_old, std::size_t* stack_size_old) noexcept;
	#endif

namespace li::platform {
	namespace {
	#if LI_CONTEXT_SANITIZER_HOOKS
		thread_local sanitizer_fiber_context* pending_sanitizer_current  = nullptr;
		thread_local sanitizer_fiber_context* pending_sanitizer_previous = nullptr;

		bool sanitizer_fiber_hooks_available() noexcept {
		#if LI_CONTEXT_SANITIZER_HOOKS_WEAK
			return __sanitizer_start_switch_fiber && __sanitizer_finish_switch_fiber;
		#else
			return true;
		#endif
		}
	#endif
	}

	extern "C" [[noreturn]] void li_context_bootstrap() noexcept;

	// The assembly bootstrap tail-calls this noexcept boundary. A callback exception is
	// therefore terminated here instead of attempting to unwind through a switched stack.
	extern "C" [[noreturn]] void li_context_run(context_function entry, void* arg, context_function completion, void* completion_arg) noexcept {
		finish_sanitizer_fiber_switch();
		entry(arg);
		if (completion)
			completion(completion_arg);
		std::terminate();
	}

	// Callers only need an address inside the current frame, so MSVC's return-address
	// slot is an adequate stand-in for reading the stack pointer register.
	std::uintptr_t current_stack_pointer() noexcept {
	#if defined(_MSC_VER) && !defined(__clang__)
		return reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
	#else
		std::uintptr_t result;
		#if LI_ARCH_ARM
		__asm__ volatile("mov %0, sp" : "=r"(result));
		#elif LI_ARCH_X86
		__asm__ volatile("movq %%rsp, %0" : "=r"(result));
		#endif
		return result;
	#endif
	}

	void start_sanitizer_fiber_switch(sanitizer_fiber_context& source, sanitizer_fiber_context& target, bool permanently_leaving) noexcept {
	#if LI_CONTEXT_SANITIZER_HOOKS
		if (!sanitizer_fiber_hooks_available())
			return;
		if (pending_sanitizer_current || pending_sanitizer_previous)
			std::terminate();

		pending_sanitizer_current  = &target;
		pending_sanitizer_previous = &source;
		__sanitizer_start_switch_fiber(permanently_leaving ? nullptr : &source.fake_stack, target.stack_bottom, target.stack_size);
	#else
		(void) source;
		(void) target;
		(void) permanently_leaving;
	#endif
	}

	void finish_sanitizer_fiber_switch() noexcept {
	#if LI_CONTEXT_SANITIZER_HOOKS
		if (!sanitizer_fiber_hooks_available() || !pending_sanitizer_current)
			return;

		sanitizer_fiber_context* current  = std::exchange(pending_sanitizer_current, nullptr);
		sanitizer_fiber_context* previous = std::exchange(pending_sanitizer_previous, nullptr);
		if (!previous)
			std::terminate();
		__sanitizer_finish_switch_fiber(current->fake_stack, &previous->stack_bottom, &previous->stack_size);
	#endif
	}

	void set_context_stack_bounds(native_context& context, void* stack_limit, void* stack_base) noexcept {
	#if LI_ABI_MS64
		context.stack_limit = reinterpret_cast<std::uintptr_t>(stack_limit);
		context.stack_base  = reinterpret_cast<std::uintptr_t>(stack_base);
	#else
		(void) context;
		(void) stack_limit;
		(void) stack_base;
	#endif
	}

	void make_context(native_context& context, void* stack_top, context_function entry, void* arg, context_function completion, void* completion_arg) noexcept {
		if (!stack_top || !entry) [[unlikely]]
			std::terminate();

		context                = native_context{};
		const auto aligned_top = reinterpret_cast<std::uintptr_t>(stack_top) & ~std::uintptr_t{15};

	#if LI_ARCH_ARM
		context.x19 = reinterpret_cast<std::uintptr_t>(entry);
		context.x20 = reinterpret_cast<std::uintptr_t>(arg);
		context.x21 = reinterpret_cast<std::uintptr_t>(completion);
		context.x22 = reinterpret_cast<std::uintptr_t>(completion_arg);
		context.sp  = aligned_top;
		context.pc  = reinterpret_cast<std::uintptr_t>(&li_context_bootstrap);
	#elif LI_ABI_SYSV64
		// The saved RSP describes the state after context_switch returns. Leave one
		// synthetic return slot so the bootstrap has ordinary SysV function-entry alignment.
		context.rsp                                     = aligned_top - sizeof(std::uintptr_t);
		*reinterpret_cast<std::uintptr_t*>(context.rsp) = 0;
		context.pc                                      = reinterpret_cast<std::uintptr_t>(&li_context_bootstrap);
		context.r12                                     = reinterpret_cast<std::uintptr_t>(entry);
		context.r13                                     = reinterpret_cast<std::uintptr_t>(arg);
		context.r14                                     = reinterpret_cast<std::uintptr_t>(completion);
		context.r15                                     = reinterpret_cast<std::uintptr_t>(completion_arg);
		context.mxcsr                                   = _mm_getcsr();
		#if LI_GNU
		__asm__ volatile("fnstcw %0" : "=m"(context.x87_control));
		#else
		context.x87_control = 0x037f;
		#endif
	#elif LI_ABI_MS64
		// Win64 function entry requires RSP = 8 (mod 16), a return slot, and 32 bytes
		// of caller-provided home space above it.

		context.rsp                                     = aligned_top - 5 * sizeof(std::uintptr_t);
		*reinterpret_cast<std::uintptr_t*>(context.rsp) = 0;
		context.pc                                      = reinterpret_cast<std::uintptr_t>(&li_context_bootstrap);
		context.r12                                     = reinterpret_cast<std::uintptr_t>(entry);
		context.r13                                     = reinterpret_cast<std::uintptr_t>(arg);

		context.r14   = reinterpret_cast<std::uintptr_t>(completion);
		context.r15   = reinterpret_cast<std::uintptr_t>(completion_arg);
		context.mxcsr = _mm_getcsr();
		set_context_stack_bounds(context, reinterpret_cast<void*>(aligned_top - 64 * 1024), reinterpret_cast<void*>(aligned_top));
		#if LI_GNU
		__asm__ volatile("fnstcw %0" : "=m"(context.x87_control));
		#else
		context.x87_control = 0x037f;
		#endif
	#endif
	}
};

	#undef LI_CONTEXT_SANITIZER_HOOKS
	#undef LI_CONTEXT_SANITIZER_HOOKS_WEAK
	#undef LI_CONTEXT_SANITIZER_WEAK

#endif
