#pragma once
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

#define LI_STRINGIFY_I(x) #x
#define LI_STRINGIFY(x)   LI_STRINGIFY_I(x)
#define LI_STRCAT_I(x, y) x##y
#define LI_STRCAT(x, y)   LI_STRCAT_I(x, y)

namespace lightning {
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
#if defined(__GNUC__) || defined(__clang__)
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