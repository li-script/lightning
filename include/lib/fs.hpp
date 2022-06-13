#pragma once
#include <util/common.hpp>
#include <lang/types.hpp>
#include <string>
#include <string_view>
#include <optional>

namespace li::lib::fs {
	using fn_import = any (*)(vm* L, std::string_view importer, std::string_view name);

	// Import hook, uses fs::read_string.
	//
	any default_import(vm* L, std::string_view importer, std::string_view name);
};

// If LI_NO_STD_FS is set, the functions below will have no definitions and
// will be required a definitiom from the user instead.
//
#ifndef LI_NO_STD_FS
	#define LI_NO_STD_FS 0
#endif

namespace li::lib::fs {
	// Reads an entire file as string. BOM can be ignored as it will handled by us.
	// Used for loading of scripts.
	//
	std::optional<std::string> read_string(const char* path);

	// TODO: Will add more when fs is exposed to the VM.
	//  Currently only used to load scripts.
	//
};