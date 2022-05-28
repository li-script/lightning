#pragma once
#include <string_view>
#include <vm/gc.hpp>
#include <vm/state.hpp>

namespace li {
	struct string : gc::leaf<string, type_string> {
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

		const char*      c_str() const { return data; }
		std::string_view view() const { return {data, length}; }
	};

	// Define forwarded vm::error.
	//
	template<typename... Tx>
	bool vm::error(const char* fmt, Tx... args) {
		if constexpr (sizeof...(Tx) != 0) {
			push_stack(string::format(this, fmt, args...));
		} else {
			push_stack(string::create(this, fmt));
		}
		return false;
	}
};