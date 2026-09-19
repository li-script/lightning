#pragma once
#include <cstddef>

namespace li::ir {
	struct procedure;
	struct value;

	struct ownership_stats {
		size_t retains_inserted      = 0;
		size_t releases_inserted     = 0;
		size_t ownership_moves       = 0;
		size_t retains_elided        = 0;
		size_t releases_elided       = 0;
		size_t pairs_elided          = 0;
		size_t edges_split           = 0;
		size_t rc_ops_before_elision = 0;
		size_t rc_ops_after_elision  = 0;
		bool   already_processed     = false;
	};

	// True when a value can never contain a dynamically reference-counted object.
	// Constants referenced by generated code are pinned by its owning code/prototype,
	// but still need ownership when returned or stored; call lowering may borrow a
	// statically known function target directly.
	bool value_may_need_reference_counting(const value* value);

	// Ownership lowering marks a vcall operand with a borrowed move when the
	// operand's owned reference is transferred into the callee frame.
	bool is_ownership_transfer(const value* value);
}

namespace li::ir::opt {
	// Runs once after all semantic optimizations and before prepare_for_mir.
	// rc_ops_before_elision includes the exact retain/release pairs replaced by
	// ownership moves; rc_ops_after_elision is counted from the final IR.
	ownership_stats ownership(procedure* proc);
}
