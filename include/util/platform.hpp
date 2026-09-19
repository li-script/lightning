#pragma once
#include <stdint.h>
#include <limits>
#include <system_error>
#include <util/common.hpp>

namespace li {
	struct vm;

	// Page allocator:
	// - f(ctx, nullptr, N, ?) =     allocation
	// - f(ctx, ptr,     N, false) = free
	// - f(ctx, ctx,     0, false) = close state
	//
	using fn_alloc = void* (*)(void* ud, void* pointer, size_t page_count, bool executable);
};

// Platform-dependant functions.
//
namespace li::platform {
	// OS mapping granularity for code_memory and native_stack; sets error on failure.
	// Keep this shared: never add per-allocator page-size queries. Windows and native x86
	// use 4 KiB; Apple ARM64 uses 16 KiB. Other Unix ARM64 kernels choose their page size
	// at boot, so only those targets need a runtime query, cached once for the process.
#if LI_WINDOWS || LI_OSX || (LI_UNIX && LI_ARCH_X86)
	[[nodiscard]] inline constexpr std::size_t native_page_size(std::error_code&) noexcept {
	#if LI_OSX && LI_ARCH_ARM
		return 16 * 1024;
	#else
		return 4 * 1024;
	#endif
	}
#else
	[[nodiscard]] std::size_t native_page_size(std::error_code& error) noexcept;
#endif

	// Rounds a mapping size to a nonzero OS page size; leaves rounded unchanged on overflow.
	[[nodiscard]] inline constexpr bool round_to_page(std::size_t size, std::size_t page_size, std::size_t& rounded) noexcept {
		const std::size_t remainder = size % page_size;
		if (remainder == 0) {
			rounded = size;
			return true;
		}
		const std::size_t padding = page_size - remainder;
		if (size > std::numeric_limits<std::size_t>::max() - padding) [[unlikely]]
			return false;
		rounded = size + padding;
		return true;
	}

	// Invoked to ensure ANSI escapes work.
	//
	void setup_ansi_escapes();

	// Default page allocator.
	//
	void* page_alloc(void* ud, void* pointer, size_t page_count, bool executable);

	// Checks if shift is being held for REPL.
	//
	bool is_shift_down();

	// Secure random generator.
	//
	uint64_t srng();
};