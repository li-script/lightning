#pragma once
#include <string_view>
#include <vm/gc.hpp>
#include <vm/state.hpp>

namespace lightning::core {
	struct string : gc_leaf<string> {
		static string* create(vm* L, std::string_view from);

		uint32_t hash;
		uint32_t length;
		char     data[];  // Null terminated, immutable after construction.

		std::string_view view() const { return {data, length}; }
	};
};