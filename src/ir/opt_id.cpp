#include <cstdint>
#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>
#include <unordered_map>
#include <vector>

namespace li::ir::opt {
	namespace detail {
		static bool is_scalar(type value_type) { return value_type == type::i1 || is_integer_data(value_type) || is_floating_point_data(value_type); }

		bool is_mathematical_scalar(const insn* instruction) {
			if (!is_scalar(instruction->vt))
				return false;
			switch (instruction->opc) {
				case opcode::unop:
					return is_scalar(instruction->operands[1]->vt);
				case opcode::binop:
				case opcode::compare:
					return is_scalar(instruction->operands[1]->vt) && is_scalar(instruction->operands[2]->vt);
				case opcode::bool_and:
				case opcode::bool_or:
				case opcode::bool_xor:
					return is_scalar(instruction->operands[0]->vt) && is_scalar(instruction->operands[1]->vt);
				case opcode::coerce_bool:
					return is_scalar(instruction->operands[0]->vt);
				case opcode::select:
					return is_scalar(instruction->operands[0]->vt) && is_scalar(instruction->operands[1]->vt) && is_scalar(instruction->operands[2]->vt);
				case opcode::struct_array_class_test:
					// A typed array's element kind and class are fixed at creation, so
					// the test is a pure function of the array reference.
					return true;
				default:
					return false;
			}
		}

		bool may_trap_target_integer(const insn* instruction) {
			if (!instruction->is<binop>() || is_floating_point_data(instruction->vt))
				return false;
			auto operation = instruction->operands[0]->as<constant>()->vmopr;
			return operation == bc::ADIV || operation == bc::AMOD;
		}
	};

	static bool is_cse_candidate(const insn* instruction) {
		// Generic arithmetic carries conservative dynamic-dispatch effects. Fully
		// scalar operands prove that those paths cannot be taken, without
		// weakening the instruction's flags for any other optimization.
		return !instruction->is_volatile && !instruction->is_terminator() && detail::is_mathematical_scalar(instruction);
	}

	static size_t expression_hash(const insn* instruction) {
		size_t hash = size_t(instruction->opc) ^ (size_t(instruction->vt) << 8);
		for (const auto& operand : instruction->operands) {
			auto bits = reinterpret_cast<uintptr_t>(operand.get());
			hash ^= size_t(bits + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
		}
		return hash;
	}

	static bool is_identical(const insn* lhs, const insn* rhs) {
		if (lhs->opc != rhs->opc || lhs->vt != rhs->vt || lhs->operands.size() != rhs->operands.size())
			return false;
		for (size_t index = 0; index != lhs->operands.size(); ++index) {
			if (lhs->operands[index].get() != rhs->operands[index].get())
				return false;
		}
		return true;
	}

	static bool precedes_in_block(const insn* definition, const insn* use) {
		for (auto* instruction : definition->parent->insns()) {
			if (instruction == definition)
				return true;
			if (instruction == use)
				return false;
		}
		return false;
	}

	static bool dominates(const insn* definition, const insn* use) {
		if (definition->parent == use->parent)
			return precedes_in_block(definition, use);
		return definition->parent->dom(use->parent);
	}

	// Applies identical value folding. Global folding is deliberately limited to
	// mathematical operations whose concrete operand types rule out heap access,
	// allocation, user dispatch, and exceptions.
	//
	void fold_identical(procedure* proc, bool local) {
		std::vector<insn*> candidates;
		for (auto& block : proc->basic_blocks) {
			for (auto* instruction : block->insns()) {
				if (is_cse_candidate(instruction))
					candidates.emplace_back(instruction);
			}
		}

		bool changed;
		do {
			std::unordered_map<size_t, std::vector<insn*>> expressions;
			for (auto* instruction : candidates)
				expressions[expression_hash(instruction)].emplace_back(instruction);

			changed = false;
			for (auto* instruction : candidates) {
				auto found = expressions.find(expression_hash(instruction));
				if (found == expressions.end())
					continue;

				for (auto* definition : found->second) {
					if (definition == instruction || (local && definition->parent != instruction->parent) || !dominates(definition, instruction) ||
						 !is_identical(definition, instruction))
						continue;
					changed |= instruction->replace_all_uses(definition) != 0;
					break;
				}
			}
		} while (changed);

		proc->validate();
	}
};