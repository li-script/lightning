#pragma once
#include <stdint.h>
#include <cstring>

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

#if defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64) || defined(__AMD_64) || defined(_M_AMD64) || defined(_M_X86) || defined(__i386__)
	#define LI_ARCH_X86  1
#elif defined(__aarch64__) || defined(_M_ARM64)
	#define LI_ARCH_ARM  1
#elif defined(__EMSCRIPTEN__)
	#define LI_ARCH_WASM 1
#else
	#error "Unknown target architecture."
#endif

#if LI_ARCH_X86
	#if defined(__SSE4_2__) && (defined(__GNUC__) || defined(__clang__))
		#define LI_HAS_CRC    1
		#define _mm_crc32_u8  __builtin_ia32_crc32qi
		#define _mm_crc32_u16 __builtin_ia32_crc32hi
		#define _mm_crc32_u32 __builtin_ia32_crc32si
		#define _mm_crc32_u64 __builtin_ia32_crc32di
	#elif _MSC_VER
		#define LI_HAS_CRC    1
		#include <intrin.h>
	#endif
#endif
#if UINTPTR_MAX == 0xFFFFFFFF
	#define LI_32 1
#endif

#define LI_STRINGIFY_I(x) #x
#define LI_STRINGIFY(x)   LI_STRINGIFY_I(x)
#define LI_STRCAT_I(x, y) x##y
#define LI_STRCAT(x, y)   LI_STRCAT_I(x, y)

namespace li {
	// Determine build mode.
	//
#ifndef LI_DEBUG
	#if NDEBUG
		#define LI_DEBUG 0
	#elif _DEBUG               
		#define LI_DEBUG 1
	#else
		#define LI_DEBUG 0
	#endif
#endif

	// Compiler specifics.
	//
#if defined(__GNUC__) || defined(__clang__) || defined(__EMSCRIPTEN__)
	#define LI_PURE      __attribute__((pure))
	#define LI_CONST     __attribute__((const))
	#define LI_FLATTEN   __attribute__((flatten))
	#define LI_COLD      __attribute__((cold, noinline, disable_tail_calls))
	#define LI_INLINE    __attribute__((always_inline))
	#define LI_NOINLINE  __attribute__((noinline))
	#define LI_ALIGN(x)  __attribute__((aligned(x)))
#elif _MSC_VER
	#define LI_PURE     
	#define LI_CONST    
	#define LI_FLATTEN  
	#define LI_INLINE   [[msvc::forceinline]]
	#define LI_NOINLINE __declspec(noinline)
	#define LI_COLD     LI_NOINLINE
	#define LI_ALIGN(x) __declspec(align(x))
#endif
	LI_INLINE inline static void breakpoint() {
#if __has_builtin(__builtin_debugtrap)
		__builtin_debugtrap();
#elif defined(_MSC_VER)
		__debugbreak();
#endif
	}
	LI_INLINE inline static constexpr void assume_that(bool condition) {
#if __has_builtin(__builtin_assume)
		__builtin_assume(condition);
#elif defined(_MSC_VER)
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