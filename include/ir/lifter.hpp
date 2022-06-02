#pragma once
#include <ir/proc.hpp>
#include <vm/function.hpp>

namespace li::ir {
	// As a first step, generates crude bytecode using load_local and store_local.
	//
	std::unique_ptr<procedure> lift_bc(vm* L, function* f);
};