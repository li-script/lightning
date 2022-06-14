#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>

namespace li::ir::opt {
	/*
	* The following algorithm is a modified version adapted from the paper:
		Braun, M., Buchwald, S., Hack, S., Leißa, R., Mallon, C., Zwinkau, A. (2013). Simple and Efficient Construction of Static Single Assignment Form. In: Jhala, R., De Bosschere, K. (eds) Compiler Construction. CC 2013. Lecture Notes in
		Computer Science, vol 7791. Springer, Berlin, Heidelberg.
	*/
	static ref<> read_variable_local(bc::reg r, basic_block* b, insn* until = nullptr) {
		for (insn* ins : view::reverse(b->before(until))) {
			if (ins->is<store_local>() && ins->operands[0]->as<constant>()->i32 == r) {
				return ins->operands[1];
			}
		}
		return nullptr;
	}
	static ref<> reread_variable_local(bc::reg r, basic_block* b) {
		for (insn* ins : view::reverse(b->insns())) {
			if (ins->is<load_local>() && ins->operands[0]->as<constant>()->i32 == r) {
				return make_ref(ins);
			}
		}
		return nullptr;
	}
	static ref<> read_variable_recursive(bc::reg r, basic_block* b);
	static ref<> read_variable(bc::reg r, basic_block* b, insn* until = nullptr) {
		auto v = read_variable_local(r, b, until);
		if (!v)
			v = read_variable_recursive(r, b);
		return v;
	}
	static ref<> try_remove_trivial_phi(ref<phi> phi) {
		ref<> same = {};
		for (auto& op : phi->operands) {
			if (op == same || op == phi)
				continue;
			if (same)
				return phi;
			same = op;
		}

		std::vector<ref<ir::phi>> phi_users;
		phi->for_each_user([&](insn* i, size_t x) {
			i->operands[x] = make_use(same.get());
			if (i->is<ir::phi>())
				phi_users.emplace_back(make_ref(i->as<ir::phi>()));
			return false;
		});
		phi->erase();

		for (auto& p : phi_users)
			if (p->parent)
				try_remove_trivial_phi(std::move(p));
		return same;
	}
	static ref<> read_variable_recursive(bc::reg r, basic_block* b) {
		// Actually load the value if it does not exist.
		//
		if (b->predecessors.empty()) {
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
		else if (b->predecessors.size() == 1) {
			return read_variable(r, b->predecessors.front());
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
			for (auto& pred : b->predecessors) {
				p->operands.emplace_back(read_variable(r, pred));
			}

			// Update the PHI.
			//
			p->update();

			// Delete the temporary.
			//
			if (tmp)
				tmp->erase();
			return try_remove_trivial_phi(std::move(p));
		}
	}

	// Lowers load/store of locals to PHI nodes and named registers.
	//
	void lift_phi(procedure* proc) {
		// Generate PHIs to replace load_local in every block except the entry point.
		//
		for (auto& bb : view::reverse(proc->basic_blocks)) {
			if (bb.get() != proc->get_entry()) {
				bb->erase_if([&](insn* ins) {
					if (ins->is<load_local>()) {
						bc::reg r = ins->operands[0]->as<constant>()->i32;
						ins->replace_all_uses(read_variable(r, bb.get(), ins));
						return true;
					}
					return false;
				});
			}
		}

		// TODO: Determine writes to arguments and push them until the end.

		// Remove all stores.
		//
		for (auto& bb : proc->basic_blocks) {
			bb->erase_if([&](insn* ins) {
				if (ins->is<store_local>() && !ins->is_volatile) {
						return true;
				}
			});
		}
		proc->validate();
	}
};