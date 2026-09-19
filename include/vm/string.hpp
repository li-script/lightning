#pragma once
#include <string_view>
#include <vm/gc.hpp>
#include <vm/rc.hpp>
#include <vm/state.hpp>

namespace li {
	struct string : gc::leaf<string, type_string> {
		// Literal creation. Every creation path returns an owned reference.
		//
		static string* create(vm* L, std::string_view from);
		static string* create(vm* L) {
			rc::retain(L->empty_string);
			return L->empty_string;
		}

		// Complex creation.
		//
		static string* format(vm* L, const char* fmt, ...);
		static string* concat(vm* L, string* a, string* b);
		static string* concat(vm* L, any* a, slot_t n);

		// String data.
		//
		msize_t hash;
		msize_t length;
		char    data[];  // Null terminated, immutable after construction.

		const char*      c_str() const { return data; }
		std::string_view view() const { return {data, length}; }
	};

	// Content semantics shared by generic values, containers, and native/JIT code.
	bool LI_CC   string_value_equals(const string* lhs, const string* rhs) noexcept;
	size_t LI_CC string_value_hash(const string* value) noexcept;

	// Replaces an allocator VM's private bootstrap registry with the dedicated
	// shared weak intern registry. The caller holds shared::heap_mutex().
	void strset_reset_shared(vm* owner);

	// Define forwarded vm::error.
	//
	template<typename... Tx>
	LI_COLD inline any_t vm::error(const char* fmt, Tx... args) {
		if constexpr (sizeof...(Tx) != 0) {
			return adopt_exception(any(string::format(this, fmt, args...)));
		} else {
			return adopt_exception(any(string::create(this, fmt)));
		}
	}
};