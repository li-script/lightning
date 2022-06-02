#pragma once
#include <ir/proc.hpp>

// All optimizations.
//
namespace li::ir::opt {
	// Step #2:
	// Lowers load/store of locals to PHI nodes and named registers.
	//
	void lift_phi(procedure* proc);
};