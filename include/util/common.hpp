#pragma once
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <type_traits>
#include <ranges>

// Compiler details.
//
#ifndef _CONSTEVAL
	#if defined(__cpp_consteval)
		#define _CONSTEVAL consteval
	#else  // ^^^ supports consteval / no consteval vvv
		#define _CONSTEVAL constexpr
	#endif
#endif
#ifndef __has_builtin
	#define __has_builtin(...) 0
#endif
#ifndef __has_attribute
	#define __has_attribute(...) 0
#endif
#ifndef __has_cpp_attribute
	#define __has_cpp_attribute(...) 0
#endif
#ifndef __has_feature
	#define __has_feature(...) 0
#endif
#ifndef __has_include
	#define __has_include(...) 0
#endif

// Detect architecture.
//
#if defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64) || defined(__AMD_64) || defined(_M_AMD64) || defined(_M_IX86) || defined(__i386)
	#define LI_ARCH_X86  1
#elif defined(__aarch64__) || defined(_M_ARM64)
	#define LI_ARCH_ARM  1
#elif defined(__EMSCRIPTEN__)
	#define LI_ARCH_WASM 1
#else
	#error "Unknown target architecture."
#endif
#if UINTPTR_MAX == 0xFFFFFFFF
	#define LI_32 1
#endif

// Detect compiler.
//
#if defined(__GNUC__) || defined(__clang__)
	#define LI_GNU 1
#else
	#define LI_GNU 0
#endif
#ifdef _MSC_VER
	#define LI_MS_EXTS 1
	#if !LI_GNU
		#define LI_MSVC 1
	#endif
#endif

// Add intrinsics.
//
#if LI_ARCH_X86
	#if defined(__SSE4_2__) && LI_GNU
		#define LI_HAS_CRC    1
		#define _mm_crc32_u8  __builtin_ia32_crc32qi
		#define _mm_crc32_u16 __builtin_ia32_crc32hi
		#define _mm_crc32_u32 __builtin_ia32_crc32si
		#define _mm_crc32_u64 __builtin_ia32_crc32di
	#elif LI_MSVC
		#define LI_HAS_CRC    1
		#include <intrin.h>
	#endif
#endif

// Detect ABI.
//
#if LI_ARCH_X86 && !LI_32
	#if _WIN32
		#define LI_ABI_MS64 1
	#else
		#define LI_ABI_SYSV64 1
	#endif
#endif
#ifndef LI_KERNEL_MODE
	#ifdef _KERNEL_MODE
		#define LI_KERNEL_MODE 1
	#endif
#endif

// Options and JIT capability.
//
#ifndef LI_FAST_MATH
	#define LI_FAST_MATH 1
#endif
#ifndef LI_JIT
	#if LI_ARCH_X86 && !LI_32
		#define LI_JIT 1
	#endif
#endif

// Common macros.
//
#define LI_STRINGIFY_I(x) #x
#define LI_STRINGIFY(x)   LI_STRINGIFY_I(x)
#define LI_STRCAT_I(x, y) x##y
#define LI_STRCAT(x, y)   LI_STRCAT_I(x, y)
#define LI_NOOP(...)
#define LI_IDENTITY(...)  __VA_ARGS__
#if LI_MS_EXTS
	#define FUNCTION_NAME __FUNCSIG__
#else
	#define FUNCTION_NAME __PRETTY_FUNCTION__
#endif

namespace li {
	namespace view  = std::views;

	// Fix emscripten's non-cpp20 compliant STL.
	//
	#if defined(__EMSCRIPTEN__)
	namespace range {
		using namespace std::ranges;

		template<typename R, typename V>
		inline static constexpr auto copy(R&& r, V&& val) {
			return std::copy(r.begin(), r.end(), std::forward<V>(val));
		}
		template<typename R, typename V>
		inline static constexpr void fill(R&& r, V&& val) {
			std::fill(r.begin(), r.end(), std::forward<V>(val));
		}
		template<typename R, typename V>
		inline static constexpr auto find(R&& r, V&& val) {
			return std::find(r.begin(), r.end(), std::forward<V>(val));
		}
		template<typename R, typename F>
		inline static constexpr auto find_if(R&& r, F&& fn) {
			return std::find_if(r.begin(), r.end(), std::forward<F>(fn));
		}
	};
	#else
	namespace range = std::ranges;
	#endif

	// Small size type.
	//
	using msize_t = uint32_t;

	// Missing bit operations.
	//
	namespace util {
		static constexpr uint64_t fill_bits(int x, int o = 0) { return (x ? (~0ull >> (64 - x)) : 0) << o; }

		template<typename T>
		static constexpr T bswap(T value) {
			if constexpr (sizeof(T) == 1) {
				return value;
			} else if constexpr (sizeof(T) == 2) {
#if __has_builtin(__builtin_bswap16)
				if (!std::is_constant_evaluated())
					return __builtin_bswap16(uint16_t(value));
#endif
				return T(uint16_t((uint16_t(value) & 0xFF) << 8) | uint16_t((uint16_t(value) & 0xFF00) >> 8));
			} else if constexpr (sizeof(T) == 4) {
#if __has_builtin(__builtin_bswap32)
				if (!std::is_constant_evaluated())
					return __builtin_bswap32(uint32_t(value));
#endif
				return T(uint32_t(bswap(uint16_t((uint32_t(value) << 16) >> 16))) << 16) | (uint32_t(bswap(uint16_t((uint32_t(value) >> 16)))));
			} else if constexpr (sizeof(T) == 8) {
#if __has_builtin(__builtin_bswap64)
				if (!std::is_constant_evaluated())
					return __builtin_bswap64(value);
#endif
				return T(uint64_t(bswap(uint32_t((uint64_t(value) << 32) >> 32))) << 32) | (uint64_t(bswap(uint32_t((uint64_t(value) >> 32)))));
			} else {
				static_assert(sizeof(T) == -1, "unexpected integer size");
			}
		}
	};

	// Implement bit-cast since many STL's lack it despite compiler support.
	//
	template<class To, typename From> requires(sizeof(To) == sizeof(From))
	inline static constexpr To bit_cast(const From& x) noexcept {
		return __builtin_bit_cast(To, x);
	}

	// Determine build mode.
	//
#ifndef LI_DEBUG
	// NDEBUG seems standard: https://en.cppreference.com/w/cpp/error/assert
	#if defined(NDEBUG) && !defined(_DEBUG)
		#define LI_DEBUG 0
	#else
		#define LI_DEBUG 1
	#endif
#endif

	// Compiler specifics.
	//
#if LI_GNU
	#define LI_PURE        __attribute__((pure))
	#define LI_CONST       __attribute__((const))
	#define LI_FLATTEN     __attribute__((flatten))
	#define LI_COLD        __attribute__((cold, noinline, disable_tail_calls))
	#define LI_INLINE      __attribute__((always_inline))
	#define LI_NOINLINE    __attribute__((noinline))
	#define LI_ALIGN(x)    __attribute__((aligned(x)))
	#define LI_TRIVIAL_ABI __attribute__((trivial_abi))
#elif LI_MSVC
	#define LI_PURE
	#define LI_CONST
	#define LI_FLATTEN
	#define LI_INLINE   [[msvc::forceinline]]
	#define LI_NOINLINE __declspec(noinline)
	#define LI_COLD     LI_NOINLINE
	#define LI_ALIGN(x) __declspec(align(x))
	#define LI_TRIVIAL_ABI
#endif
	LI_INLINE inline static void breakpoint() {
#if __has_builtin(__builtin_debugtrap)
		__builtin_debugtrap();
#elif LI_MSVC
		__debugbreak();
#endif
	}
	LI_INLINE inline static constexpr void assume_that(bool condition) {
#if __has_builtin(__builtin_assume)
		__builtin_assume(condition);
#elif LI_MSVC
		__assume(condition);
#endif
	}
	LI_INLINE inline static void assume_unreachable [[noreturn]] () {
#if __has_builtin(__builtin_unreachable)
		__builtin_unreachable();
#else
		assume_that(false);
#endif
	}
};