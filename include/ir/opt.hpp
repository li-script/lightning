#pragma once
#include <ir/proc.hpp>

// All optimizations.
//
namespace li::ir::opt {
	// Lowers load/store of locals to PHI nodes and named registers.
	//
	void lift_phi(procedure* proc);

	// Applies identical value folding.
	//
	void fold_identical(procedure* proc, bool local = false);

	// Applies dead code elimination.
	//
	void dce(procedure* proc, bool local = false);

	// Optimizes the control flow graph.
	//
	void cfg(procedure* proc);

	// Infers constant type information and optimizes the control flow.
	//
	void type_inference(procedure* proc);
};