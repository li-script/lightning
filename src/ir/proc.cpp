#include <ir/proc.hpp>

namespace li::ir {
	// Recursive value copy.
	//
	static ref<> recursive_value_copy(value* a, procedure* proc) {
		// Rewrite and launder all constants.
		//
		if (a->is<constant>()) {
			constant c = *a->as<constant>();
			if (c.vt == type::bb) {
				c.bb = (basic_block*) c.bb->visited;
			}
			return launder_value(proc, c);
		}

		// If already duplicated, return.
		//
		auto* i = a->as<insn>();
		if (i->visited)
			return make_ref((insn*)i->visited);

		// Duplicate the instruction.
		//
		ref<insn> dup(std::in_place, i->duplicate());
		i->visited = (uintptr_t) dup.get();

		// Fix each operand.
		//
		for (auto& op : dup->operands) {
			op = recursive_value_copy(op, proc);
		}

		// Return the final result.
		//
		return dup;
	}

	// Duplicates the procedure.
	//
	std::unique_ptr<procedure> procedure::duplicate() {
		clear_all_visitor_state();

		// Create a new procedure.
		//
		auto result = std::make_unique<procedure>(L, f);

		// Copy basic state.
		//
		result->next_reg_name = next_reg_name;
		result->next_block_uid         = next_block_uid;
		result->is_topologically_sorted = is_topologically_sorted;

		// Pre-allocate the basic blocks, map using visitor_context.
		//
		for (size_t i = 0; i != basic_blocks.size(); i++) {
			basic_block* newbb               = result->basic_blocks.emplace_back(std::make_unique<basic_block>(result.get())).get();
			basic_blocks[i]->visited = (uintptr_t) newbb;
		}

		// For each basic block:
		//
		for (size_t i = 0; i != basic_blocks.size(); i++) {
			// Copy the basic block state.
			//
			auto bf = basic_blocks[i].get();
			auto bb = result->basic_blocks[i].get();
			bb->uid        = bf->uid * 100;
			bb->cold_hint  = bf->cold_hint;
			bb->loop_depth = bf->loop_depth;
			bb->bc_begin   = bf->bc_begin;
			bb->bc_end     = bf->bc_end;
			for (auto& suc : bf->successors)
				bb->successors.emplace_back((basic_block*) suc->visited);
			for (auto& pred : bf->predecessors)
				bb->predecessors.emplace_back((basic_block*) pred->visited);

			// Copy instructions.
			//
			for (auto i : bf->insns()) {
				bb->push_back(recursive_value_copy(i, result.get()));
			}
		}

		// Validate and return.
		//
		result->validate();
		return result;
	}
};