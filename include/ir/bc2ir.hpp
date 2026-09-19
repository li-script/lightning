#pragma once
#include <ir/proc.hpp>
#include <optional>
#include <vm/function.hpp>

namespace li::ir {
	// Lifts bytecode to SSA. With osr_target set to a loop-header bytecode
	// position, the procedure gains an on-stack-replacement entry that adopts the
	// interpreter frame's locals and continues at that header.
	std::unique_ptr<procedure> lift_bc(vm* L, function_proto* f, std::optional<bc::pos> osr_target = std::nullopt);
};