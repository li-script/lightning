#pragma once
#include <string_view>
#include <vm/gc.hpp>
#include <vm/state.hpp>

namespace lightning::core {
	struct string : gc_leaf<string> {
		// Literal creation.
		//
		static string* create(vm* L, std::string_view from);
		static string* create(vm* L) { return L->empty_string; }

		// Complex creation.
		//
		static string* format(vm* L, const char* fmt, ...);
		static string* concat(vm* L, string* a, string* b);

		// String data.
		//
		uint32_t hash;
		uint32_t length;
		char     data[];  // Null terminated, immutable after construction.

		std::string_view view() const { return {data, length}; }
	};
};