#pragma once
#include <optional>
#include <string>

namespace li::ir {
	struct basic_block;
	struct insn;
	struct procedure;

	struct verification_error {
		std::string        message;
		const basic_block* block       = nullptr;
		const insn*        instruction = nullptr;

		std::string describe() const;
	};

	// LI_DEBUG enables validation unconditionally. Release builds may opt in with
	// LI_IR_VERIFY=1; the environment is sampled once on first use.
	bool ir_verification_enabled();

	// Verifies and refreshes instruction-derived type/effect annotations. Returns
	// the first violation without terminating so native tests and embedders can
	// inspect the offending block and instruction.
	std::optional<verification_error> verify_ir(procedure& proc);
}
