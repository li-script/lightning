#pragma once
#include <ir/proc.hpp>
#include <vm/string.hpp>

namespace li::ir::jit {
	// Initial codegen with no profiling information.
	// - On success, writes the JIT callback into function and returns null.
	// - On failure, returns the error reason.
	//
#if LI_JIT
	string* generate_code(procedure* proc);
#else
	inline string* generate_code(procedure* proc) {
		return string::create(proc->L, "target does not have JIT capabilities.");
	}
#endif
};