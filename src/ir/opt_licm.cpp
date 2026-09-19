#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace li::ir::opt {
	struct natural_loop {
		basic_block*                     header = nullptr;
		std::unordered_set<basic_block*> blocks;
	};

	static bool precedes_in_block(const insn* definition, const insn* use) {
		LI_ASSERT(definition->parent == use->parent);
		for (auto* instruction : definition->parent->insns()) {
			if (instruction == definition)
				return true;
			if (instruction == use)
				return false;
		}
		return false;
	}

	static bool is_hoistable(const insn* instruction) {
		// Concrete scalar operands discharge the generic dynamic-dispatch effect
		// flags without mutating them. No heap object or user code is reachable
		// from these mathematical operations.
		if (instruction->is_volatile || instruction->is_terminator() || !detail::is_mathematical_scalar(instruction))
			return false;

		// Integer division and remainder may trap in the target ISA. Floating-point
		// division follows IEEE semantics once scalar specialization proves that
		// no dynamic dispatch or language exception is possible.
		return !detail::may_trap_target_integer(instruction);
	}

	static std::vector<natural_loop> find_natural_loops(procedure* proc) {
		std::vector<natural_loop>                loops;
		std::unordered_map<basic_block*, size_t> loop_indices;

		for (auto& tail_owner : proc->basic_blocks) {
			auto* tail = tail_owner.get();
			for (auto* header : tail->successors) {
				if (!header->dom(tail))
					continue;

				auto [position, inserted] = loop_indices.emplace(header, loops.size());
				if (inserted)
					loops.push_back(natural_loop{header, {header}});
				auto& loop = loops[position->second];

				std::vector<basic_block*> worklist;
				if (loop.blocks.emplace(tail).second && tail != header)
					worklist.emplace_back(tail);
				while (!worklist.empty()) {
					auto* block = worklist.back();
					worklist.pop_back();
					for (auto* predecessor : block->predecessors) {
						if (loop.blocks.emplace(predecessor).second && predecessor != header)
							worklist.emplace_back(predecessor);
					}
				}
			}
		}
		return loops;
	}

	static std::vector<size_t> outside_edges(const natural_loop& loop) {
		std::vector<size_t> result;
		for (size_t index = 0; index != loop.header->predecessors.size(); ++index) {
			if (!loop.blocks.contains(loop.header->predecessors[index]))
				result.emplace_back(index);
		}
		return result;
	}

	static basic_block* find_existing_preheader(const natural_loop& loop, const std::vector<size_t>& outside) {
		if (outside.size() != 1)
			return nullptr;
		auto* predecessor = loop.header->predecessors[outside.front()];
		if (predecessor->successors.size() != 1 || predecessor->successors.front() != loop.header || !predecessor->back()->is<jmp>())
			return nullptr;
		return predecessor;
	}

	static bool definition_available_in_block(const insn* definition, const basic_block* block) {
		if (definition->parent == block)
			return precedes_in_block(definition, block->back());
		return definition->parent->dom(block);
	}

	static bool operand_available_at_preheader(value* operand, const natural_loop& loop, const std::vector<size_t>& outside, basic_block* existing_preheader,
		 const std::unordered_set<insn*>& selected) {
		if (!operand->is<insn>())
			return true;
		auto* definition = operand->as<insn>();
		if (selected.contains(definition))
			return true;
		if (loop.blocks.contains(definition->parent))
			return false;

		if (existing_preheader)
			return definition_available_in_block(definition, existing_preheader);
		for (auto index : outside) {
			if (!definition_available_in_block(definition, loop.header->predecessors[index]))
				return false;
		}
		return true;
	}

	static std::vector<insn*> find_invariants(procedure* proc, const natural_loop& loop, const std::vector<size_t>& outside, basic_block* existing_preheader) {
		std::unordered_set<insn*> selected;
		std::vector<insn*>        ordered;

		bool changed;
		do {
			changed = false;
			for (auto& block_owner : proc->basic_blocks) {
				auto* block = block_owner.get();
				if (!loop.blocks.contains(block))
					continue;
				for (auto* instruction : block->insns()) {
					if (selected.contains(instruction) || !is_hoistable(instruction))
						continue;
					bool invariant = true;
					for (const auto& operand : instruction->operands) {
						if (!operand_available_at_preheader(operand.get(), loop, outside, existing_preheader, selected)) {
							invariant = false;
							break;
						}
					}
					if (invariant) {
						selected.emplace(instruction);
						ordered.emplace_back(instruction);
						changed = true;
					}
				}
			}
		} while (changed);
		return ordered;
	}

	static size_t find_successor_occurrence(const basic_block* predecessor, const basic_block* target, size_t occurrence) {
		for (size_t index = 0; index != predecessor->successors.size(); ++index) {
			if (predecessor->successors[index] == target && occurrence-- == 0)
				return index;
		}
		LI_ASSERT(false);
		return 0;
	}

	static void redirect_edge(procedure* proc, basic_block* predecessor, basic_block* from, basic_block* to, size_t occurrence) {
		auto successor_index                     = find_successor_occurrence(predecessor, from, occurrence);
		predecessor->successors[successor_index] = to;

		auto*  terminator = predecessor->back();
		size_t operand_index;
		if (terminator->is<jmp>()) {
			LI_ASSERT(successor_index == 0);
			operand_index = 0;
		} else {
			LI_ASSERT(terminator->is<jcc>() && successor_index < 2);
			operand_index = successor_index + 1;
		}
		terminator->operands[operand_index] = proc->add_const(constant(to));
	}

	static basic_block* create_preheader(procedure* proc, const natural_loop& loop, const std::vector<size_t>& outside) {
		auto* header         = loop.header;
		auto* preheader      = proc->add_block();
		preheader->cold_hint = header->cold_hint;
		preheader->bc_begin  = header->bc_begin;
		preheader->bc_end    = header->bc_begin;

		std::vector<basic_block*> old_predecessors = header->predecessors;
		std::vector<bool>         is_outside(old_predecessors.size(), false);
		for (auto index : outside) {
			is_outside[index] = true;
			preheader->predecessors.emplace_back(old_predecessors[index]);
		}

		for (auto index : outside) {
			auto* predecessor = old_predecessors[index];
			// Every edge from this predecessor is outside the loop. Redirecting
			// the first remaining occurrence preserves their original order.
			redirect_edge(proc, predecessor, header, preheader, 0);
		}

		std::vector<basic_block*> rewritten_predecessors;
		rewritten_predecessors.reserve(old_predecessors.size() - outside.size() + 1);
		bool inserted_preheader = false;
		for (size_t index = 0; index != old_predecessors.size(); ++index) {
			if (is_outside[index]) {
				if (!inserted_preheader) {
					rewritten_predecessors.emplace_back(preheader);
					inserted_preheader = true;
				}
			} else {
				rewritten_predecessors.emplace_back(old_predecessors[index]);
			}
		}

		for (auto* header_phi : header->phis()) {
			LI_ASSERT(header_phi->operands.size() == old_predecessors.size());
			value* first = header_phi->operands[outside.front()].get();
			bool   same  = true;
			for (auto index : outside)
				same &= header_phi->operands[index].get() == first;

			ref<> incoming;
			if (same) {
				incoming = make_ref(first);
			} else {
				auto merged = builder{preheader}.emit<phi>();
				for (auto index : outside)
					merged->operands.emplace_back(header_phi->operands[index]);
				merged->update();
				incoming = merged;
			}

			std::vector<use<>> rewritten_operands;
			rewritten_operands.reserve(rewritten_predecessors.size());
			bool inserted_incoming = false;
			for (size_t index = 0; index != old_predecessors.size(); ++index) {
				if (is_outside[index]) {
					if (!inserted_incoming) {
						rewritten_operands.emplace_back(incoming);
						inserted_incoming = true;
					}
				} else {
					rewritten_operands.emplace_back(header_phi->operands[index]);
				}
			}
			header_phi->operands = std::move(rewritten_operands);
			header_phi->update();
		}

		header->predecessors = std::move(rewritten_predecessors);
		preheader->successors.emplace_back(header);
		builder{preheader}.emit<jmp>(header);
		proc->mark_blocks_dirty();
		return preheader;
	}

	static void optimize_loop(procedure* proc, const natural_loop& loop) {
		auto outside = outside_edges(loop);
		if (outside.empty())
			return;

		auto* preheader  = find_existing_preheader(loop, outside);
		auto  invariants = find_invariants(proc, loop, outside, preheader);
		if (invariants.empty())
			return;
		if (!preheader)
			preheader = create_preheader(proc, loop, outside);

		auto* terminator = preheader->back();
		for (auto* instruction : invariants)
			preheader->insert(instruction_iterator(terminator), instruction->erase());
	}

	void licm(procedure* proc) {
		std::unordered_set<basic_block*> processed;
		while (true) {
			auto   loops         = find_natural_loops(proc);
			size_t selected      = loops.size();
			size_t selected_size = 0;
			for (size_t index = 0; index != loops.size(); ++index) {
				if (processed.contains(loops[index].header) || loops[index].blocks.size() <= selected_size)
					continue;
				selected      = index;
				selected_size = loops[index].blocks.size();
			}
			if (selected == loops.size())
				break;
			processed.emplace(loops[selected].header);
			optimize_loop(proc, loops[selected]);
		}
		proc->validate();
	}
};
