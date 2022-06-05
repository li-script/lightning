#pragma once
#include <ir/proc.hpp>
#include <vm/function.hpp>

namespace li::ir {
	// Generates crude bytecode.
	//
	std::unique_ptr<procedure> lift_bc(vm* L, function* f);
};