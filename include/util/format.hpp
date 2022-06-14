#pragma once
#include "common.hpp"
#include <string>
#include <stdarg.h>
#include <util/utf.hpp>

namespace li::util {
	#define LI_BRG  "\x1B[1;37m"
	#define LI_YLW  "\x1B[1;33m"
	#define LI_PRP  "\x1B[1;35m"
	#define LI_RED  "\x1B[1;31m"
	#define LI_CYN  "\x1B[1;36m"
	#define LI_GRN  "\x1B[1;32m"
	#define LI_BLU  "\x1B[1;34m"
	#define LI_DEF  "\x1B[0m"

	// Length of string without the ANSI escapes.
	//
	inline static size_t display_length(std::string_view s) {
		size_t result = 0;
		while (true) {
			auto p = s.find("\x1B[");
			result += utf_length(s.substr(0, p));
			if (p == std::string::npos) {
				break;
			}
			s.remove_prefix(p+2);
			size_t n = 0;
			if (!s.empty()) {
				n = s[0] == '1' ? 5 : 2; 
			}
			s.remove_prefix(std::min(s.size(), n));
		}
		return result;
	}

	// Formats as std::string.
	//
	inline static std::string fmt(const char* fmt, ...) {
		static constexpr size_t small_capacity = 32;

		va_list a1;
		va_start(a1, fmt);

		va_list a2;
		va_copy(a2, a1);
		std::string buffer;
		buffer.resize(small_capacity);
		buffer.resize(vsnprintf(buffer.data(), small_capacity + 1, fmt, a2));
		va_end(a2);
		if (buffer.size() <= small_capacity) {
			va_end(a1);
			return buffer;
		} else {
			vsnprintf(buffer.data(), buffer.size() + 1, fmt, a1);
			va_end(a1);
			return buffer;
		}
	}

	// Asserts and errors.
	//
	LI_COLD inline static void abort [[noreturn]] (const char* fmt, ...) {
		va_list args;
		va_start(args, fmt);
		vprintf(fmt, args);
		va_end(args);
		::abort();
	}

#if LI_DEBUG
	#define LI_ASSERT(...)                                                                                                            \
		do                                                                                                                             \
			if (!(__VA_ARGS__)) [[unlikely]]                                                                                            \
				li::util::abort("Assertion \"" LI_STRINGIFY(__VA_ARGS__) "\" failed. [" __FILE__ ":" LI_STRINGIFY(__LINE__) "]\n"); \
		while (0)
	#define LI_ASSERT_MSG(msg, ...)                                                   \
		do                                                                             \
			if (!(__VA_ARGS__)) [[unlikely]]                                            \
				li::util::abort(msg "[" __FILE__ ":" LI_STRINGIFY(__LINE__) "]\n"); \
		while (0)
#else
	#define LI_ASSERT(...)          li::assume_that(__VA_ARGS__)
	#define LI_ASSERT_MSG(msg, ...) li::assume_that(__VA_ARGS__)
#endif
};