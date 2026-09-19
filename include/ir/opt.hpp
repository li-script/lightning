#pragma once
#include <ir/mir.hpp>
#include <ir/ownership.hpp>
#include <ir/proc.hpp>

//
// -- SSA IR Optimizations --
//
namespace li::ir::opt {
	namespace detail {
		// Shared proof used by scalar optimizations. It recognizes only
		// operations whose concrete operand types exclude dynamic dispatch.
		bool is_mathematical_scalar(const insn* instruction);
		bool may_trap_target_integer(const insn* instruction);
	};

	// Lowers load/store of locals to PHI nodes and named registers.
	//
	void lift_phi(procedure* proc);

	// Applies constant folding.
	//
	void fold_constant(procedure* proc);

	// Applies identical value folding.
	//
	void fold_identical(procedure* proc, bool local = false);

	// Applies dead code elimination.
	//
	void dce(procedure* proc, bool local = false);

	// Optimizes the control flow graph.
	//
	void cfg(procedure* proc);

	// Adds the branches for required type checks.
	//
	void type_split_cfg(procedure* proc);

	// Infers constant type information and optimizes the control flow.
	//
	void type_inference(procedure* proc);

	// Hoists loop-invariant, effect-free scalar operations.
	//
	void licm(procedure* proc);

	// Replaces proven non-escaping aggregates with scalar SSA values.
	size_t scalar_replace(procedure* proc);

	// Inlines known scalar script calls within a bounded instruction budget.
	size_t inline_calls(procedure* proc, size_t instruction_budget = 512);

	// Specializes exact, bounded integral arithmetic without changing number semantics.
	size_t specialize_integer_ranges(procedure* proc);

	// Prepares the IR to be lifted to MIR.
	//
	void prepare_for_mir(procedure* proc);
	void finalize_for_mir(procedure* proc);
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