#pragma once
#include <vm/function.hpp>

namespace lightning::core {
	// Parses the code and returns it as a function instance with no arguments on success.
	// If code parsing fails, result is instead a string explaining the error.
	//
	any load_script(core::vm* L, std::string_view source);
};