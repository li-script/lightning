#pragma once
#include <vm/function.hpp>

namespace li {
	// Parses the code and returns it as a function instance with no arguments on success.
	// If code parsing fails, result is instead a string explaining the error.
	//
	any load_script(vm* L, std::string_view source, std::string_view source_name = "string", bool disable_locals = false);
};