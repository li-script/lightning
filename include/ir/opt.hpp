#pragma once
#include <ir/proc.hpp>
#include <ir/mir.hpp>

//
// -- SSA IR Optimizations --
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

//
// -- Machine IR Optimizations --
//
namespace li::ir::opt {
	// Attempts to optimize-out any SETCC's by moving them nearby to the JS.
	//
	void remove_redundant_setcc(mprocedure* proc);

	// Allocates registers for each virtual register and generates the spill instructions.
	//
	void allocate_registers(mprocedure* proc);
};