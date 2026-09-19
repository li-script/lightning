#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>
#include <unordered_set>
#include <vector>

namespace li::ir::opt {
	static bool is_discardable(const insn* instruction) {
		if (instruction->is_terminator() || instruction->is_volatile)
			return false;

		// Scalar specialization proves that generic dispatch effects are
		// unreachable. Integer division and remainder remain observable traps.
		if (detail::is_mathematical_scalar(instruction))
			return !detail::may_trap_target_integer(instruction);

		// Calls, allocations, and lifetime operations remain roots even if a
		// future flag refinement makes their generic effects less conservative.
		switch (instruction->opc) {
			case opcode::array_new:
			case opcode::table_new:
			case opcode::class_new:
			case opcode::keep_alive:
			case opcode::retain:
			case opcode::release:
			case opcode::ccall:
			case opcode::vcall:
				return false;
			default:
				return instruction->is_pure && !instruction->sideffect && instruction->effects == effect_none;
		}
	}

	// Applies whole-procedure dead code elimination. Marking from observable
	// instructions removes dead dependency cycles as well as linear dead chains.
	//
	void dce(procedure* proc, bool) {
		std::unordered_set<insn*> live;
		std::vector<insn*>        worklist;

		for (auto& block : proc->basic_blocks) {
			for (auto* instruction : block->insns()) {
				if (!is_discardable(instruction) && live.emplace(instruction).second)
					worklist.emplace_back(instruction);
			}
		}

		while (!worklist.empty()) {
			auto* instruction = worklist.back();
			worklist.pop_back();
			for (const auto& operand : instruction->operands) {
				if (operand->is<insn>()) {
					auto* definition = operand->as<insn>();
					if (live.emplace(definition).second)
						worklist.emplace_back(definition);
				}
			}
		}

		std::vector<ref<insn>> dead;
		for (auto& block : proc->basic_blocks) {
			for (auto* instruction : block->insns()) {
				if (is_discardable(instruction) && !live.contains(instruction))
					dead.emplace_back(make_ref(instruction));
			}
		}

		// Drop operands as a group before unlinking instructions so dead PHI
		// cycles and other mutually-referential values release cleanly.
		for (auto& instruction : dead)
			instruction->operands.clear();
		for (auto& instruction : dead)
			instruction->erase();

		proc->validate();
	}
};