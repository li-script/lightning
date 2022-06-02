#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>

namespace li::ir::opt {

	static value_ref read_variable_local(bc::reg r, basic_block* b, insn* until = nullptr) {
		for (auto it = b->instructions.rbegin(); it != b->instructions.rend(); ++it) {
			auto& ins = *it;

			if (until) {
				if (ins.get() != until) {
					continue;
				}
				else {
					until = nullptr;
					continue;
				}
			}

			if (ins->op == opcode::store_local && ins->operands[0]->get<constant>()->i == r) {
				return ins->operands[1];
			}
		}
		return nullptr;
	}
	static value_ref reread_variable_local(bc::reg r, basic_block* b) {
		for (auto it = b->instructions.rbegin(); it != b->instructions.rend(); ++it) {
			auto& ins = *it;
			if (ins->op == opcode::load_local && ins->operands[0]->get<constant>()->i == r) {
				return ins;
			}
		}
		return nullptr;
	}

	// Braun11CC
	//
	static value_ref read_variable_recursive(bc::reg r, basic_block* b);
	static value_ref read_variable(bc::reg r, basic_block* b, insn* until = nullptr) {
		auto v = read_variable_local(r, b, until);
		if (!v)
			v = read_variable_recursive(r, b);
		return v;
	}

	// TODO: tryRemoveTrivialPhi

	static value_ref try_remove_trivial_phi(value_ref phi) {
		value_ref same = {};
		for (auto& op : phi->get<insn>()->operands) {
			if (op == same || op == phi)
				continue;
			if (same)
				return phi;
			same = op;
		}
		// users = phi.users.remove(phi)
		auto blk = phi->get<insn>()->parent;
		blk->proc->replace_all_uses(phi.get(), same);
		std::erase(blk->instructions, phi);
		// for use in users:
		//  if use is phi:
		//   tryremovetrivialbla
		return same;
	}

	static value_ref read_variable_recursive(bc::reg r, basic_block* b) {
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
			insn_ref p = bd.emit_front<phi>();

			// Create a temporary store to break cycles if necessary
			//
			insn_ref tmp;
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
				if (b->instructions.back() == tmp)
					b->instructions.pop_back();
				else
					std::erase(b->instructions, tmp);
			}
			return try_remove_trivial_phi(p);
		}
	}

	// Step #2:
	// Lowers load/store of locals to PHI nodes and named registers.
	//
	void lift_phi(procedure* proc) {
		// Generate PHIs to replace load_local in every block except the entry point.
		//
		for (auto it = proc->basic_blocks.rbegin(); it != std::prev(proc->basic_blocks.rend()); ++it) {
			auto& bb = *it;

			std::erase_if(bb->instructions, [&](insn_ref& ins) {
				if (ins->op == opcode::load_local) {
					bc::reg r = ins->operands[0]->get<constant>()->i;
					bb->replace_all_uses(ins.get(), read_variable(r, bb.get(), ins.get()));
					return true;
				}
				return false;
			});
		}

		// TODO: Determine writes to arguments and push them until the end.

		// Remove stores to internal variables.
		//
		for (auto& bb : proc->basic_blocks) {
			std::erase_if(bb->instructions, [&](insn_ref& ins) {
				if (ins->op == opcode::store_local && !ins->is_volatile) {
					bc::reg r = ins->operands[0]->get<constant>()->i;
					if (r >= 0)
						return true;
				}
				return false;
			});
		}
		
		// Dead simple DCE.
		//
		while (true) {
			size_t n = 0;
			for (auto& bb : proc->basic_blocks) {
				n += std::erase_if(bb->instructions, [&](insn_ref& ins) {
					if (ins.use_count() == 1 && ins->vt != type::none && !ins->is_volatile) {
						return true;
					}
					return false;
				});
			}
			if (!n)
				break;
		};

		proc->rename_registers();
		proc->validate();
		proc->print();
	}
};