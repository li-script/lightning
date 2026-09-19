#pragma once

// These offsets are consumed by the preprocessed assembly implementations. Keep them as
// integer preprocessor constants and prove the C++ layout against them below.
#define LI_CONTEXT_A64_X19_OFFSET 0
#define LI_CONTEXT_A64_X20_OFFSET 8
#define LI_CONTEXT_A64_X21_OFFSET 16
#define LI_CONTEXT_A64_X22_OFFSET 24
#define LI_CONTEXT_A64_X23_OFFSET 32
#define LI_CONTEXT_A64_X24_OFFSET 40
#define LI_CONTEXT_A64_X25_OFFSET 48
#define LI_CONTEXT_A64_X26_OFFSET 56
#define LI_CONTEXT_A64_X27_OFFSET 64
#define LI_CONTEXT_A64_X28_OFFSET 72
#define LI_CONTEXT_A64_X29_OFFSET 80
#define LI_CONTEXT_A64_X30_OFFSET 88
#define LI_CONTEXT_A64_SP_OFFSET  96
#define LI_CONTEXT_A64_D8_OFFSET  104
#define LI_CONTEXT_A64_D9_OFFSET  112
#define LI_CONTEXT_A64_D10_OFFSET 120
#define LI_CONTEXT_A64_D11_OFFSET 128
#define LI_CONTEXT_A64_D12_OFFSET 136
#define LI_CONTEXT_A64_D13_OFFSET 144
#define LI_CONTEXT_A64_D14_OFFSET 152
#define LI_CONTEXT_A64_D15_OFFSET 160
#define LI_CONTEXT_A64_SIZE       176

#define LI_CONTEXT_SYSV64_RBX_OFFSET   0
#define LI_CONTEXT_SYSV64_RBP_OFFSET   8
#define LI_CONTEXT_SYSV64_R12_OFFSET   16
#define LI_CONTEXT_SYSV64_R13_OFFSET   24
#define LI_CONTEXT_SYSV64_R14_OFFSET   32
#define LI_CONTEXT_SYSV64_R15_OFFSET   40
#define LI_CONTEXT_SYSV64_RSP_OFFSET   48
#define LI_CONTEXT_SYSV64_PC_OFFSET    56
#define LI_CONTEXT_SYSV64_MXCSR_OFFSET 64
#define LI_CONTEXT_SYSV64_X87CW_OFFSET 68
#define LI_CONTEXT_SYSV64_SIZE         80

#define LI_CONTEXT_WIN64_RBX_OFFSET        0
#define LI_CONTEXT_WIN64_RBP_OFFSET        8
#define LI_CONTEXT_WIN64_RDI_OFFSET        16
#define LI_CONTEXT_WIN64_RSI_OFFSET        24
#define LI_CONTEXT_WIN64_R12_OFFSET        32
#define LI_CONTEXT_WIN64_R13_OFFSET        40
#define LI_CONTEXT_WIN64_R14_OFFSET        48
#define LI_CONTEXT_WIN64_R15_OFFSET        56
#define LI_CONTEXT_WIN64_RSP_OFFSET        64
#define LI_CONTEXT_WIN64_PC_OFFSET         72
#define LI_CONTEXT_WIN64_MXCSR_OFFSET      80
#define LI_CONTEXT_WIN64_X87CW_OFFSET      84
#define LI_CONTEXT_WIN64_XMM6_OFFSET       96
#define LI_CONTEXT_WIN64_XMM7_OFFSET       112
#define LI_CONTEXT_WIN64_XMM8_OFFSET       128
#define LI_CONTEXT_WIN64_XMM9_OFFSET       144
#define LI_CONTEXT_WIN64_XMM10_OFFSET      160
#define LI_CONTEXT_WIN64_XMM11_OFFSET      176
#define LI_CONTEXT_WIN64_XMM12_OFFSET      192
#define LI_CONTEXT_WIN64_XMM13_OFFSET      208
#define LI_CONTEXT_WIN64_XMM14_OFFSET      224
#define LI_CONTEXT_WIN64_XMM15_OFFSET      240
#define LI_CONTEXT_WIN64_STACKBASE_OFFSET  256
#define LI_CONTEXT_WIN64_STACKLIMIT_OFFSET 264
#define LI_CONTEXT_WIN64_SIZE              272

#ifndef __ASSEMBLER__

	#include <cstddef>
	#include <cstdint>
	#include <type_traits>
	#include <util/common.hpp>

	#ifndef LI_CONTEXT_SUPPORTED
		#if (LI_ARCH_ARM && !LI_32 && !LI_WINDOWS && (LI_OSX || LI_UNIX)) || (LI_ARCH_X86 && !LI_32 && LI_ABI_SYSV64 && (LI_OSX || LI_UNIX)) || \
			 (LI_ARCH_X86 && !LI_32 && LI_ABI_MS64 && LI_GNU)
			#define LI_CONTEXT_SUPPORTED 1
		#else
			#define LI_CONTEXT_SUPPORTED 0
		#endif
	#endif

namespace li::platform {
	using context_function = void (*)(void*);

	// Per-stack state required by the sanitizer fiber-switch interface. stack_bottom
	// and stack_size describe the currently accessible portion of a downward-growing
	// stack; fake_stack is opaque and is populated by the sanitizer runtime.
	struct sanitizer_fiber_context {
		void*       fake_stack   = nullptr;
		const void* stack_bottom = nullptr;
		std::size_t stack_size   = 0;
	};

	#if LI_ARCH_ARM && !LI_32 && !LI_WINDOWS
	// AAPCS64 preserves x19-x29, the link register used as the resumed PC, SP, and
	// the low 64 bits of v8-v15. The explicit tail padding makes the assembly size
	// independent of compiler padding choices.
	struct alignas(16) native_context {
		std::uintptr_t x19 = 0;
		std::uintptr_t x20 = 0;
		std::uintptr_t x21 = 0;
		std::uintptr_t x22 = 0;
		std::uintptr_t x23 = 0;
		std::uintptr_t x24 = 0;
		std::uintptr_t x25 = 0;
		std::uintptr_t x26 = 0;
		std::uintptr_t x27 = 0;
		std::uintptr_t x28 = 0;
		std::uintptr_t x29 = 0;
		union {
			std::uintptr_t x30 = 0;
			std::uintptr_t pc;
		};
		std::uintptr_t sp       = 0;
		std::uint64_t  d8       = 0;
		std::uint64_t  d9       = 0;
		std::uint64_t  d10      = 0;
		std::uint64_t  d11      = 0;
		std::uint64_t  d12      = 0;
		std::uint64_t  d13      = 0;
		std::uint64_t  d14      = 0;
		std::uint64_t  d15      = 0;
		std::uint64_t  reserved = 0;
	};

	inline constexpr bool native_context_supported = LI_CONTEXT_SUPPORTED != 0;

	static_assert(offsetof(native_context, x19) == LI_CONTEXT_A64_X19_OFFSET);
	static_assert(offsetof(native_context, x20) == LI_CONTEXT_A64_X20_OFFSET);
	static_assert(offsetof(native_context, x21) == LI_CONTEXT_A64_X21_OFFSET);
	static_assert(offsetof(native_context, x22) == LI_CONTEXT_A64_X22_OFFSET);
	static_assert(offsetof(native_context, x23) == LI_CONTEXT_A64_X23_OFFSET);
	static_assert(offsetof(native_context, x24) == LI_CONTEXT_A64_X24_OFFSET);
	static_assert(offsetof(native_context, x25) == LI_CONTEXT_A64_X25_OFFSET);
	static_assert(offsetof(native_context, x26) == LI_CONTEXT_A64_X26_OFFSET);
	static_assert(offsetof(native_context, x27) == LI_CONTEXT_A64_X27_OFFSET);
	static_assert(offsetof(native_context, x28) == LI_CONTEXT_A64_X28_OFFSET);
	static_assert(offsetof(native_context, x29) == LI_CONTEXT_A64_X29_OFFSET);
	static_assert(offsetof(native_context, x30) == LI_CONTEXT_A64_X30_OFFSET);
	static_assert(offsetof(native_context, pc) == LI_CONTEXT_A64_X30_OFFSET);
	static_assert(offsetof(native_context, sp) == LI_CONTEXT_A64_SP_OFFSET);
	static_assert(offsetof(native_context, d8) == LI_CONTEXT_A64_D8_OFFSET);
	static_assert(offsetof(native_context, d9) == LI_CONTEXT_A64_D9_OFFSET);
	static_assert(offsetof(native_context, d10) == LI_CONTEXT_A64_D10_OFFSET);
	static_assert(offsetof(native_context, d11) == LI_CONTEXT_A64_D11_OFFSET);
	static_assert(offsetof(native_context, d12) == LI_CONTEXT_A64_D12_OFFSET);
	static_assert(offsetof(native_context, d13) == LI_CONTEXT_A64_D13_OFFSET);
	static_assert(offsetof(native_context, d14) == LI_CONTEXT_A64_D14_OFFSET);
	static_assert(offsetof(native_context, d15) == LI_CONTEXT_A64_D15_OFFSET);
	static_assert(sizeof(native_context) == LI_CONTEXT_A64_SIZE);
	#elif LI_ARCH_X86 && !LI_32 && LI_ABI_SYSV64
	struct alignas(16) native_context {
		std::uintptr_t rbx         = 0;
		std::uintptr_t rbp         = 0;
		std::uintptr_t r12         = 0;
		std::uintptr_t r13         = 0;
		std::uintptr_t r14         = 0;
		std::uintptr_t r15         = 0;
		std::uintptr_t rsp         = 0;
		std::uintptr_t pc          = 0;
		std::uint32_t  mxcsr       = 0;
		std::uint16_t  x87_control = 0;
		std::byte      reserved[10]{};
	};

	inline constexpr bool native_context_supported = LI_CONTEXT_SUPPORTED != 0;

	static_assert(offsetof(native_context, rbx) == LI_CONTEXT_SYSV64_RBX_OFFSET);
	static_assert(offsetof(native_context, rbp) == LI_CONTEXT_SYSV64_RBP_OFFSET);
	static_assert(offsetof(native_context, r12) == LI_CONTEXT_SYSV64_R12_OFFSET);
	static_assert(offsetof(native_context, r13) == LI_CONTEXT_SYSV64_R13_OFFSET);
	static_assert(offsetof(native_context, r14) == LI_CONTEXT_SYSV64_R14_OFFSET);
	static_assert(offsetof(native_context, r15) == LI_CONTEXT_SYSV64_R15_OFFSET);
	static_assert(offsetof(native_context, rsp) == LI_CONTEXT_SYSV64_RSP_OFFSET);
	static_assert(offsetof(native_context, pc) == LI_CONTEXT_SYSV64_PC_OFFSET);
	static_assert(offsetof(native_context, mxcsr) == LI_CONTEXT_SYSV64_MXCSR_OFFSET);
	static_assert(offsetof(native_context, x87_control) == LI_CONTEXT_SYSV64_X87CW_OFFSET);
	static_assert(sizeof(native_context) == LI_CONTEXT_SYSV64_SIZE);
	#elif LI_ARCH_X86 && !LI_32 && LI_ABI_MS64
	struct alignas(16) native_xmm_register {
		std::uint64_t low  = 0;
		std::uint64_t high = 0;
	};

	struct alignas(16) native_context {
		std::uintptr_t      rbx         = 0;
		std::uintptr_t      rbp         = 0;
		std::uintptr_t      rdi         = 0;
		std::uintptr_t      rsi         = 0;
		std::uintptr_t      r12         = 0;
		std::uintptr_t      r13         = 0;
		std::uintptr_t      r14         = 0;
		std::uintptr_t      r15         = 0;
		std::uintptr_t      rsp         = 0;
		std::uintptr_t      pc          = 0;
		std::uint32_t       mxcsr       = 0;
		std::uint16_t       x87_control = 0;
		std::byte           reserved[10]{};
		native_xmm_register xmm6{};
		native_xmm_register xmm7{};
		native_xmm_register xmm8{};
		native_xmm_register xmm9{};
		native_xmm_register xmm10{};
		native_xmm_register xmm11{};
		native_xmm_register xmm12{};
		native_xmm_register xmm13{};
		native_xmm_register xmm14{};
		native_xmm_register xmm15{};
		std::uintptr_t      stack_base  = 0;
		std::uintptr_t      stack_limit = 0;
	};

	inline constexpr bool native_context_supported = LI_CONTEXT_SUPPORTED != 0;

	static_assert(offsetof(native_context, rbx) == LI_CONTEXT_WIN64_RBX_OFFSET);
	static_assert(offsetof(native_context, rbp) == LI_CONTEXT_WIN64_RBP_OFFSET);
	static_assert(offsetof(native_context, rdi) == LI_CONTEXT_WIN64_RDI_OFFSET);
	static_assert(offsetof(native_context, rsi) == LI_CONTEXT_WIN64_RSI_OFFSET);
	static_assert(offsetof(native_context, r12) == LI_CONTEXT_WIN64_R12_OFFSET);
	static_assert(offsetof(native_context, r13) == LI_CONTEXT_WIN64_R13_OFFSET);
	static_assert(offsetof(native_context, r14) == LI_CONTEXT_WIN64_R14_OFFSET);
	static_assert(offsetof(native_context, r15) == LI_CONTEXT_WIN64_R15_OFFSET);
	static_assert(offsetof(native_context, rsp) == LI_CONTEXT_WIN64_RSP_OFFSET);
	static_assert(offsetof(native_context, pc) == LI_CONTEXT_WIN64_PC_OFFSET);
	static_assert(offsetof(native_context, mxcsr) == LI_CONTEXT_WIN64_MXCSR_OFFSET);
	static_assert(offsetof(native_context, x87_control) == LI_CONTEXT_WIN64_X87CW_OFFSET);
	static_assert(offsetof(native_context, xmm6) == LI_CONTEXT_WIN64_XMM6_OFFSET);
	static_assert(offsetof(native_context, xmm7) == LI_CONTEXT_WIN64_XMM7_OFFSET);
	static_assert(offsetof(native_context, xmm8) == LI_CONTEXT_WIN64_XMM8_OFFSET);
	static_assert(offsetof(native_context, xmm9) == LI_CONTEXT_WIN64_XMM9_OFFSET);
	static_assert(offsetof(native_context, xmm10) == LI_CONTEXT_WIN64_XMM10_OFFSET);
	static_assert(offsetof(native_context, xmm11) == LI_CONTEXT_WIN64_XMM11_OFFSET);
	static_assert(offsetof(native_context, xmm12) == LI_CONTEXT_WIN64_XMM12_OFFSET);
	static_assert(offsetof(native_context, xmm13) == LI_CONTEXT_WIN64_XMM13_OFFSET);
	static_assert(offsetof(native_context, xmm14) == LI_CONTEXT_WIN64_XMM14_OFFSET);
	static_assert(offsetof(native_context, xmm15) == LI_CONTEXT_WIN64_XMM15_OFFSET);
	static_assert(offsetof(native_context, stack_base) == LI_CONTEXT_WIN64_STACKBASE_OFFSET);
	static_assert(offsetof(native_context, stack_limit) == LI_CONTEXT_WIN64_STACKLIMIT_OFFSET);
	static_assert(sizeof(native_context) == LI_CONTEXT_WIN64_SIZE);
	#else
	// The type remains nameable so platform-independent owners can contain it, but no
	// switch implementation is provided on unsupported targets.
	struct native_context {};
	inline constexpr bool native_context_supported = false;
	#endif

	static_assert(std::is_standard_layout_v<native_context>);
	static_assert(std::is_trivially_copyable_v<native_context>);

	#if LI_CONTEXT_SUPPORTED
	// Returns the architectural stack pointer. Unlike the address of a local, this
	// cannot be redirected to AddressSanitizer's fake stack.
	[[nodiscard]] std::uintptr_t current_stack_pointer() noexcept;

	// Brackets a raw context_switch for sanitizer runtimes that expose the fiber
	// switching interface. Call finish_sanitizer_fiber_switch immediately after
	// context_switch returns. The bootstrap does this automatically for a context's
	// first entry. permanently_leaving destroys the source's fake stack.
	void start_sanitizer_fiber_switch(sanitizer_fiber_context& source, sanitizer_fiber_context& target, bool permanently_leaving = false) noexcept;
	void finish_sanitizer_fiber_switch() noexcept;

	// Suspends the current native frame into from and resumes to. The two contexts
	// must belong to this thread. No exception may cross a suspended native frame.
	extern "C" void context_switch(native_context* from, const native_context* to) noexcept;

	// Updates the bounds associated with a suspended context. On Win64 these become the
	// current NT_TIB StackLimit/StackBase when the context resumes, allowing ordinary
	// stack probing and exception handling inside the fiber. Other ABIs need no action.
	void set_context_stack_bounds(native_context& context, void* stack_limit, void* stack_base) noexcept;

	// Prepares a never-started context on a writable downward-growing stack. stack_top
	// is the byte immediately above the stack and is rounded down to 16 bytes. If entry
	// returns, completion is called with completion_arg. Returning from completion (or
	// omitting it) terminates; a completion normally switches to its owning scheduler.
	// Neither callback is allowed to unwind through the native context boundary. Win64
	// callers must replace the conservative initial 64 KiB limit via set_context_stack_bounds
	// before the first switch and whenever the suspended stack's committed low end changes.
	void make_context(native_context& context, void* stack_top, context_function entry, void* arg, context_function completion = nullptr,
		 void* completion_arg = nullptr) noexcept;
	#endif
};

#endif
