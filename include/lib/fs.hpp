#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <util/common.hpp>
#include <vm/types.hpp>

namespace li::util {
	struct native_function;
}

namespace li::lib {
	// Registers the trusted-script filesystem module.
	//
	void register_fs(vm* L);
}

namespace li::lib::fs {
	// Import hooks return one owned value on success. nil means not found and
	// exception_marker means vm::last_ex owns the failure payload.
	using fn_import = any (*)(vm* L, std::string_view importer, std::string_view name);

	// Import hook, uses fs::read_string.
	//
	any default_import(vm* L, std::string_view importer, std::string_view name);

	namespace detail {
		// Private native helper embedded in import bytecode.
		extern util::native_function module_import;
	}
};

// If LI_NO_STD_FS is set, register_fs and read_string have no definitions and
// must be supplied by the embedder if used.
//
#ifndef LI_NO_STD_FS
	#define LI_NO_STD_FS 0
#endif

namespace li::lib::fs {
	// Reads an entire file as string. BOM can be ignored as it will handled by us.
	// Used for loading of scripts.
	//
	std::optional<std::string> read_string(const char* path);
};