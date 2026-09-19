#include <util/context.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#if LI_ARCH_X86
	#include <xmmintrin.h>
#endif
#if LI_WINDOWS
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#define WIN32_LEAN_AND_MEAN
	#include <Windows.h>
#endif

#if defined(__APPLE__)
	#define LI_TEST_ASM_SYMBOL(name) "_" #name
#else
	#define LI_TEST_ASM_SYMBOL(name) #name
#endif

namespace context_switch_test {
	void require(bool condition, std::string_view message) {
		if (!condition)
			throw std::runtime_error(std::string(message));
	}

#if LI_ARCH_ARM
	struct alignas(16) register_sentinel {
		std::array<std::uint64_t, 4> gp{};
		std::array<std::uint64_t, 4> fp{};
	};

	static_assert(offsetof(register_sentinel, gp) == 0);
	static_assert(offsetof(register_sentinel, fp) == 32);
	static_assert(sizeof(register_sentinel) == 64);

	register_sentinel sentinel(std::uint64_t seed) {
		return {{
						seed ^ 0x1919191919191919ull,
						seed ^ 0x2020202020202020ull,
						seed ^ 0x2727272727272727ull,
						seed ^ 0x2828282828282828ull,
				  },
			 {
				  seed ^ 0xd8d8d8d8d8d8d8d8ull,
				  seed ^ 0xdbdbdbdbdbdbdbdbull,
				  seed ^ 0xdedededededededeull,
				  seed ^ 0xdfdfdfdfdfdfdfdfull,
			 }};
	}

	bool same_sentinel(const register_sentinel& left, const register_sentinel& right) { return left.gp == right.gp && left.fp == right.fp; }
#elif LI_ABI_SYSV64
	struct alignas(16) register_sentinel {
		std::array<std::uint64_t, 5> gp{};
		std::uint32_t                mxcsr       = 0;
		std::uint16_t                x87_control = 0;
		std::uint16_t                reserved    = 0;
	};

	static_assert(offsetof(register_sentinel, gp) == 0);
	static_assert(offsetof(register_sentinel, mxcsr) == 40);
	static_assert(offsetof(register_sentinel, x87_control) == 44);
	static_assert(sizeof(register_sentinel) == 48);

	register_sentinel sentinel(std::uint64_t seed) {
		register_sentinel result{{
			 seed ^ 0xbbbbbbbbbbbbbbbbull,
			 seed ^ 0x1212121212121212ull,
			 seed ^ 0x1313131313131313ull,
			 seed ^ 0x1414141414141414ull,
			 seed ^ 0x1515151515151515ull,
		}};
		result.mxcsr = (_mm_getcsr() & ~0x6000u) | (static_cast<std::uint32_t>(seed >> 12) & 0x6000u);
	#if LI_GNU
		__asm__ volatile("fnstcw %0" : "=m"(result.x87_control));
	#else
		result.x87_control = 0x037f;
	#endif
		result.x87_control = static_cast<std::uint16_t>((result.x87_control & ~0x0c00u) | (static_cast<std::uint16_t>(seed >> 10) & 0x0c00u));
		return result;
	}

	bool same_sentinel(const register_sentinel& left, const register_sentinel& right) {
		return left.gp == right.gp && left.mxcsr == right.mxcsr && left.x87_control == right.x87_control;
	}
#elif LI_ABI_MS64
	struct alignas(16) register_sentinel {
		std::array<std::uint64_t, 7> gp{};
		std::uint32_t                mxcsr       = 0;
		std::uint16_t                x87_control = 0;
		std::uint16_t                reserved    = 0;
		alignas(16) std::array<std::array<std::uint64_t, 2>, 4> xmm{};
	};

	static_assert(offsetof(register_sentinel, gp) == 0);
	static_assert(offsetof(register_sentinel, mxcsr) == 56);
	static_assert(offsetof(register_sentinel, x87_control) == 60);
	static_assert(offsetof(register_sentinel, xmm) == 64);
	static_assert(sizeof(register_sentinel) == 128);

	register_sentinel sentinel(std::uint64_t seed) {
		register_sentinel result{{
			 seed ^ 0xbbbbbbbbbbbbbbbbull,
			 seed ^ 0xd1d1d1d1d1d1d1d1ull,
			 seed ^ 0x5151515151515151ull,
			 seed ^ 0x1212121212121212ull,
			 seed ^ 0x1313131313131313ull,
			 seed ^ 0x1414141414141414ull,
			 seed ^ 0x1515151515151515ull,
		}};
		result.mxcsr = (_mm_getcsr() & ~0x6000u) | (static_cast<std::uint32_t>(seed >> 12) & 0x6000u);
	#if LI_GNU
		__asm__ volatile("fnstcw %0" : "=m"(result.x87_control));
	#else
		result.x87_control = 0x037f;
	#endif
		result.x87_control = static_cast<std::uint16_t>((result.x87_control & ~0x0c00u) | (static_cast<std::uint16_t>(seed >> 10) & 0x0c00u));
		for (std::size_t i = 0; i != result.xmm.size(); ++i) {
			result.xmm[i][0] = seed ^ (0x0606060606060606ull + i);
			result.xmm[i][1] = seed ^ (0xf6f6f6f6f6f6f6f6ull - i);
		}
		return result;
	}

	bool same_sentinel(const register_sentinel& left, const register_sentinel& right) {
		return left.gp == right.gp && left.mxcsr == right.mxcsr && left.x87_control == right.x87_control && left.xmm == right.xmm;
	}
#endif
};

#if LI_GNU && LI_ARCH_ARM
asm(
	".text\n"
	".p2align 2\n"
	".globl " LI_TEST_ASM_SYMBOL(li_test_context_switch) "\n"
	LI_TEST_ASM_SYMBOL(li_test_context_switch) ":\n"
	"sub sp, sp, #96\n"
	"stp x29, x30, [sp, #0]\n"
	"stp x19, x20, [sp, #16]\n"
	"stp x27, x28, [sp, #32]\n"
	"stp d8, d11, [sp, #48]\n"
	"stp d14, d15, [sp, #64]\n"
	"str x3, [sp, #80]\n"
	"ldp x19, x20, [x2, #0]\n"
	"ldp x27, x28, [x2, #16]\n"
	"ldp d8, d11, [x2, #32]\n"
	"ldp d14, d15, [x2, #48]\n"
	"bl " LI_TEST_ASM_SYMBOL(context_switch) "\n"
	"ldr x4, [sp, #80]\n"
	"stp x19, x20, [x4, #0]\n"
	"stp x27, x28, [x4, #16]\n"
	"stp d8, d11, [x4, #32]\n"
	"stp d14, d15, [x4, #48]\n"
	"ldp d14, d15, [sp, #64]\n"
	"ldp d8, d11, [sp, #48]\n"
	"ldp x27, x28, [sp, #32]\n"
	"ldp x19, x20, [sp, #16]\n"
	"ldp x29, x30, [sp, #0]\n"
	"add sp, sp, #96\n"
	"ret\n"
);
#elif LI_GNU && LI_ABI_SYSV64
asm(
	".text\n"
	".p2align 4, 0x90\n"
	".globl " LI_TEST_ASM_SYMBOL(li_test_context_switch) "\n"
	LI_TEST_ASM_SYMBOL(li_test_context_switch) ":\n"
	"pushq %rbx\n"
	"pushq %r12\n"
	"pushq %r13\n"
	"pushq %r14\n"
	"pushq %r15\n"
	"subq $32, %rsp\n"
	"movq %rcx, 0(%rsp)\n"
	"stmxcsr 8(%rsp)\n"
	"fnstcw 12(%rsp)\n"
	"movq 0(%rdx), %rbx\n"
	"movq 8(%rdx), %r12\n"
	"movq 16(%rdx), %r13\n"
	"movq 24(%rdx), %r14\n"
	"movq 32(%rdx), %r15\n"
	"ldmxcsr 40(%rdx)\n"
	"fldcw 44(%rdx)\n"
	"callq " LI_TEST_ASM_SYMBOL(context_switch) "\n"
	"movq 0(%rsp), %rax\n"
	"movq %rbx, 0(%rax)\n"
	"movq %r12, 8(%rax)\n"
	"movq %r13, 16(%rax)\n"
	"movq %r14, 24(%rax)\n"
	"movq %r15, 32(%rax)\n"
	"stmxcsr 40(%rax)\n"
	"fnstcw 44(%rax)\n"
	"ldmxcsr 8(%rsp)\n"
	"fldcw 12(%rsp)\n"
	"addq $32, %rsp\n"
	"popq %r15\n"
	"popq %r14\n"
	"popq %r13\n"
	"popq %r12\n"
	"popq %rbx\n"
	"retq\n"
);
#elif LI_GNU && LI_ABI_MS64
asm(
	".text\n"
	".p2align 4, 0x90\n"
	".globl " LI_TEST_ASM_SYMBOL(li_test_context_switch) "\n"
	LI_TEST_ASM_SYMBOL(li_test_context_switch) ":\n"
	"pushq %rbx\n"
	"pushq %rdi\n"
	"pushq %rsi\n"
	"pushq %r12\n"
	"pushq %r13\n"
	"pushq %r14\n"
	"pushq %r15\n"
	"subq $112, %rsp\n"
	"movq %r9, 32(%rsp)\n"
	"stmxcsr 40(%rsp)\n"
	"fnstcw 44(%rsp)\n"
	"movdqu %xmm6, 48(%rsp)\n"
	"movdqu %xmm7, 64(%rsp)\n"
	"movdqu %xmm12, 80(%rsp)\n"
	"movdqu %xmm15, 96(%rsp)\n"
	"movq 0(%r8), %rbx\n"
	"movq 8(%r8), %rdi\n"
	"movq 16(%r8), %rsi\n"
	"movq 24(%r8), %r12\n"
	"movq 32(%r8), %r13\n"
	"movq 40(%r8), %r14\n"
	"movq 48(%r8), %r15\n"
	"ldmxcsr 56(%r8)\n"
	"fldcw 60(%r8)\n"
	"movdqu 64(%r8), %xmm6\n"
	"movdqu 80(%r8), %xmm7\n"
	"movdqu 96(%r8), %xmm12\n"
	"movdqu 112(%r8), %xmm15\n"
	"callq " LI_TEST_ASM_SYMBOL(context_switch) "\n"
	"movq 32(%rsp), %rax\n"
	"movq %rbx, 0(%rax)\n"
	"movq %rdi, 8(%rax)\n"
	"movq %rsi, 16(%rax)\n"
	"movq %r12, 24(%rax)\n"
	"movq %r13, 32(%rax)\n"
	"movq %r14, 40(%rax)\n"
	"movq %r15, 48(%rax)\n"
	"stmxcsr 56(%rax)\n"
	"fnstcw 60(%rax)\n"
	"movdqu %xmm6, 64(%rax)\n"
	"movdqu %xmm7, 80(%rax)\n"
	"movdqu %xmm12, 96(%rax)\n"
	"movdqu %xmm15, 112(%rax)\n"
	"movdqu 48(%rsp), %xmm6\n"
	"movdqu 64(%rsp), %xmm7\n"
	"movdqu 80(%rsp), %xmm12\n"
	"movdqu 96(%rsp), %xmm15\n"
	"ldmxcsr 40(%rsp)\n"
	"fldcw 44(%rsp)\n"
	"addq $112, %rsp\n"
	"popq %r15\n"
	"popq %r14\n"
	"popq %r13\n"
	"popq %r12\n"
	"popq %rsi\n"
	"popq %rdi\n"
	"popq %rbx\n"
	"retq\n"
);
#endif

namespace context_switch_test {
#if LI_GNU || LI_ABI_MS64
	extern "C" void li_test_context_switch(
		 li::platform::native_context* from, const li::platform::native_context* to, const register_sentinel* expected, register_sentinel* observed) noexcept;

	void switch_with_probe(li::platform::native_context& from, const li::platform::native_context& to, li::platform::sanitizer_fiber_context& from_fiber,
		 li::platform::sanitizer_fiber_context& to_fiber, const register_sentinel& expected) {
		register_sentinel observed{};
		li::platform::start_sanitizer_fiber_switch(from_fiber, to_fiber);
		li_test_context_switch(&from, &to, &expected, &observed);
		li::platform::finish_sanitizer_fiber_switch();
		require(same_sentinel(observed, expected), "callee-saved register state changed across context switch");
	}
#else
	void switch_with_probe(li::platform::native_context& from, const li::platform::native_context& to, li::platform::sanitizer_fiber_context& from_fiber,
		 li::platform::sanitizer_fiber_context& to_fiber, const register_sentinel& expected) {
		const std::uint32_t original_mxcsr = _mm_getcsr();
		_mm_setcsr(expected.mxcsr);
		li::platform::start_sanitizer_fiber_switch(from_fiber, to_fiber);
		li::platform::context_switch(&from, &to);
		li::platform::finish_sanitizer_fiber_switch();
		const std::uint32_t observed_mxcsr = _mm_getcsr();
		_mm_setcsr(original_mxcsr);
		require(observed_mxcsr == expected.mxcsr, "MXCSR changed across context switch");
	}
#endif

	struct probe_state {
		li::platform::native_context          main_context{};
		li::platform::native_context          fiber_context{};
		li::platform::sanitizer_fiber_context main_fiber{};
		li::platform::sanitizer_fiber_context fiber{};
		std::uintptr_t                        magic                = 0x7a31b59dc4e2068full;
		const volatile std::uintptr_t*        stack_cookie_address = nullptr;
		void*                                 expected_stack_limit = nullptr;
		void*                                 expected_stack_base  = nullptr;
		std::size_t                           yields               = 0;
		bool                                  argument_received    = false;
		bool                                  exception_caught     = false;
		bool                                  completed            = false;
	};

	void fiber_entry(void* opaque) {
		auto& state             = *static_cast<probe_state*>(opaque);
		state.argument_received = state.magic == 0x7a31b59dc4e2068full;
#if LI_WINDOWS
		auto* const tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
		require(tib->StackLimit == state.expected_stack_limit, "fiber NT_TIB StackLimit was not installed");
		require(tib->StackBase == state.expected_stack_base, "fiber NT_TIB StackBase was not installed");
#endif
		try {
			throw std::runtime_error("fiber-local unwind probe");
		} catch (const std::runtime_error&) {
			state.exception_caught = true;
		}

		const auto stack_pointer = li::platform::current_stack_pointer();
		const auto stack_low     = reinterpret_cast<std::uintptr_t>(state.expected_stack_limit);
		const auto stack_high    = reinterpret_cast<std::uintptr_t>(state.expected_stack_base);
		require(stack_pointer >= stack_low && stack_pointer <= stack_high, "architectural SP is outside the fiber stack");

		volatile std::uintptr_t stack_cookie = state.magic ^ 0x55aa55aa55aa55aaull;
		state.stack_cookie_address           = &stack_cookie;
		for (std::size_t i = 0; i != 4; ++i) {
			require(state.stack_cookie_address == &stack_cookie, "fiber native frame moved between switches");
			require(stack_cookie == (state.magic ^ 0x55aa55aa55aa55aaull), "fiber stack local was not preserved");
			++state.yields;
			switch_with_probe(state.fiber_context, state.main_context, state.fiber, state.main_fiber, sentinel(0xf100000000000000ull + i));
		}
	}

	void fiber_complete(void* opaque) noexcept {
		auto& state     = *static_cast<probe_state*>(opaque);
		state.completed = true;
		li::platform::start_sanitizer_fiber_switch(state.fiber, state.main_fiber, true);
		li::platform::context_switch(&state.fiber_context, &state.main_context);
		std::terminate();
	}

	void run_probe() {
		static constexpr std::size_t stack_size = 128 * 1024;
		probe_state                  state{};
		auto                         stack = std::make_unique<std::byte[]>(stack_size);
		state.expected_stack_limit         = stack.get();
		state.expected_stack_base          = stack.get() + stack_size;
		state.fiber.stack_bottom           = stack.get();
		state.fiber.stack_size             = stack_size;
		const auto main_stack_pointer      = li::platform::current_stack_pointer();
		const auto stack_low               = reinterpret_cast<std::uintptr_t>(state.expected_stack_limit);
		const auto stack_high              = reinterpret_cast<std::uintptr_t>(state.expected_stack_base);
		require(main_stack_pointer < stack_low || main_stack_pointer > stack_high, "main architectural SP overlaps the fiber stack");
#if LI_WINDOWS
		auto* const main_tib         = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
		void* const main_stack_limit = main_tib->StackLimit;
		void* const main_stack_base  = main_tib->StackBase;
#endif
		li::platform::make_context(state.fiber_context, stack.get() + stack_size, fiber_entry, &state, fiber_complete, &state);
		li::platform::set_context_stack_bounds(state.fiber_context, stack.get(), stack.get() + stack_size);

		for (std::size_t i = 0; i != 5; ++i) {
			switch_with_probe(state.main_context, state.fiber_context, state.main_fiber, state.fiber, sentinel(0xa200000000000000ull + i));
#if LI_WINDOWS
			require(main_tib->StackLimit == main_stack_limit, "main NT_TIB StackLimit was not restored");
			require(main_tib->StackBase == main_stack_base, "main NT_TIB StackBase was not restored");
#endif
		}

		require(state.argument_received, "bootstrap did not transfer the entry argument");
		require(state.exception_caught, "fiber-local native exception did not unwind on the fiber stack");
		require(state.yields == 4, "fiber did not resume at each suspended native frame");
		require(state.completed, "returning entry did not invoke its completion continuation");
	}
};

int main() {
	static_assert(li::platform::native_context_supported);
	context_switch_test::run_probe();
	return 0;
}
