#pragma once
#include <ir/mir.hpp>
#include <ir/proc.hpp>

namespace li::ir {
	// Generates crude machine IR from the SSA IR.
	//
	std::unique_ptr<mprocedure> lift_ir(procedure* p);

	// Assembles the pseudo-target instructions in the IR.
	//
	void assemble_ir(mprocedure* proc);
};