#pragma once
#include <ir/mir.hpp>
#include <ir/proc.hpp>
#include <string>

namespace li::ir {
	// Generates crude machine IR from the SSA IR.
	//
	std::unique_ptr<mprocedure> lift_ir(procedure* p);

	// Assembles neutral MIR for the selected native target.
	//
	jfunction* assemble_ir(mprocedure* proc);

	// Disassembles exactly the published code bytes owned by the JIT function.
	//
	std::string disassemble_code(const jfunction& function);
};