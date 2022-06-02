#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>

namespace li::ir::opt {
	static ref<> read_variable_local(bc::reg r, basic_block* b, insn* until = nullptr) {
		for (auto it = b->rbegin(); it != b->rend(); ++it) {
			auto ins = *it;

			if (until) {
				if (ins != until) {
					continue;
				}
				else {
					until = nullptr;
					continue;
				}
			}
			if (ins->is<store_local>() && ins->operands[0]->as<constant>()->i32 == r) {
				return ins->operands[1];
			}
		}
		return nullptr;
	}
	static ref<> reread_variable_local(bc::reg r, basic_block* b) {
		for (auto it = b->rbegin(); it != b->rend(); ++it) {
			auto ins = *it;
			if (ins->is<load_local>() && ins->operands[0]->as<constant>()->i32 == r) {
				return make_ref(ins);
			}
		}
		return nullptr;
	}

	// Braun11CC
	//
	static ref<> read_variable_recursive(bc::reg r, basic_block* b);
	static ref<> read_variable(bc::reg r, basic_block* b, insn* until = nullptr) {
		auto v = read_variable_local(r, b, until);
		if (!v)
			v = read_variable_recursive(r, b);
		return v;
	}

	// TODO: tryRemoveTrivialPhi

	static ref<> try_remove_trivial_phi(ref<phi> phi) {
		ref<> same = {};
		for (auto& op : phi->operands) {
			if (op == same || op == phi)
				continue;
			if (same)
				return phi;
			same = op;
		}
		// users = phi.users.remove(phi)
		auto blk = phi->parent;
		phi->replace_all_uses(same);
		phi->erase();
		// for use in users:
		//  if use is phi:
		//   tryremovetrivialbla
		return same;
	}

	static ref<> read_variable_recursive(bc::reg r, basic_block* b) {
		// Actually load the value if it does not exist.
		//
		if (b->predecesors.empty()) {
			LI_ASSERT(r < 0);
			auto v = read_variable_local(r, b);
			if (!v) {
				v = reread_variable_local(r, b);
				if (!v) {
					builder bd{b};
					v = bd.emit_front<load_local>(r);
				}
			}
			return v;
		}
		// No PHI needed.
		//
		else if (b->predecesors.size() == 1) {
			return read_variable(r, b->predecesors.front());
		}
		// Create a PHI recursively.
		//
		else {
			// Create an empty phi.
			//
			builder bd{b};
			ref<insn> p = bd.emit_front<phi>();

			// Create a temporary store to break cycles if necessary
			//
			ref<insn> tmp;
			if (!read_variable_local(r, b)) {
				tmp = bd.emit<store_local>(r, p);
			}

			// For each predecessor, append a PHI node.
			//
			for (auto& pred : b->predecesors) {
				p->operands.emplace_back(read_variable(r, pred));
			}

			// Update the PHI.
			//
			p->update();

			// Delete the temporary.
			//
			if (tmp) {
				b->erase(std::move(tmp));
			}
			return try_remove_trivial_phi(std::move(p));
		}
	}

	// Lowers load/store of locals to PHI nodes and named registers.
	//
	void lift_phi(procedure* proc) {
		// Generate PHIs to replace load_local in every block except the entry point.
		//
		for (auto it = proc->basic_blocks.rbegin(); it != std::prev(proc->basic_blocks.rend()); ++it) {
			auto& bb = *it;

			bb->erase_if([&](insn* ins) {
				if (ins->is<load_local>()) {
					bc::reg r = ins->operands[0]->as<constant>()->i32;
					ins->replace_all_uses(read_variable(r, bb.get(), ins));
					return true;
				}
				return false;
			});
		}

		// TODO: Determine writes to arguments and push them until the end.

		// Remove stores to internal variables.
		//
		for (auto& bb : proc->basic_blocks) {
			bb->erase_if([&](insn* ins) {
				if (ins->is<store_local>() && !ins->is_volatile) {
					bc::reg r = ins->operands[0]->as<constant>()->i32;
					if (r >= -FRAME_SIZE)
						return true;
				}
				return false;
			});
		}
		proc->validate();
	}
};