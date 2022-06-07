#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>
#include <variant>
#include <vm/traits.hpp>

namespace li::ir::opt {
	// Resolves dominating type for a value at the given instruction.
	//
	static std::optional<value_type> get_dominating_type_at(insn* i, value* v) {
		if (v->vt != type::unk) {
			return to_vm_type(v->vt);
		} else {
			std::optional<value_type> resolved;
			v->as<insn>()->for_each_user([&](insn* c, size_t j) {
				// TODD: test_type branch check, has path to.
				if (c->is<assume_cast>()) {
					if (c->parent->dom(i->parent)) {
						resolved = to_vm_type(c->operands[1]->as<constant>()->irtype);
						return true;
					}
				}
				return false;
			});
			return resolved;
		}
	}

	// Splits the instruction stream into two, returns two instructions in a block with only the splitting instruction, an assume cast if applicable and the jmp.
	//
	static std::pair<ref<insn>, ref<insn>> split_by(insn* i, size_t op, std::variant<value_type, trait> check_against) {
		// Insert the type check before the instruction.
		//
		builder b{i};
		ref<insn> cc;
		if (check_against.index() == 0)
			cc = b.emit_before<test_type>(i, i->operands[op], std::get<0>(check_against));
		else
			cc = b.emit_before<test_trait>(i, i->operands[op], std::get<1>(check_against));

		// Split the block after the check, add the jcc.
		//
		basic_block* at        = i->parent;
		auto         cont_blk  = at->split_at(cc);
		auto         true_blk  = at->proc->add_block();
		auto         false_blk = at->proc->add_block();
		b.emit<jcc>(cc, true_blk, false_blk);
		at->proc->add_jump(at, true_blk);
		at->proc->add_jump(at, false_blk);
		//true_blk->visited  = at->visited;
		//false_blk->visited = at->visited;

		// Copy type into two.
		//
		auto tunchecked        = make_ref(i->duplicate());
		auto tchecked          = i->erase();
		if (check_against.index() == 0)
			tchecked->operands[op] = builder{true_blk}.emit<assume_cast>(i->operands[op], to_ir_type(std::get<0>(check_against)));

		// Create both blocks.
		//
		auto v1 = true_blk->push_back(tchecked);
		auto v2 = false_blk->push_back(tunchecked);
		tchecked->update();
		builder{true_blk}.emit<jmp>(cont_blk);
		builder{false_blk}.emit<jmp>(cont_blk);
		true_blk->proc->add_jump(true_blk, cont_blk);
		false_blk->proc->add_jump(false_blk, cont_blk);

		// Create a PHI at the destination if there is a result and replace all uses.
		//
		if (v1.at->vt != type::none) {
			auto ph = builder{cont_blk}.emit_front<phi>(v1.at, v2.at);
			tchecked->for_each_user_outside_block([&](insn* i, size_t op) {
				if (i != ph)
					i->operands[op].reset(ph);
				return false;
			});
		}
		i->parent->validate();
		return {std::move(tchecked), std::move(tunchecked)};
	}

	// Each specialization.
	//
	static void specialize_op(procedure* proc) {
		proc->bfs([&](basic_block* bb) {
			// Find the first numeric operation with an unknown type.
			//
			auto i = range::find_if(bb->insns(), [](insn* i) {
				if (i->is<binop>() || i->is<compare>()) {
					i->update();
					if (i->is<compare>() && (i->operands[1]->vt != i->operands[2]->vt)) {
						return true;
					}
					if (i->vt != type::unk) {
						for (auto& op : i->operands)
							op->update();
					} else {
						return true;
					}
				}
				return false;
			});
			if (i == bb->end()) {
				return false;
			}

			auto [y, f] = split_by(i.at, i->operands[1]->vt == type::unk ? 1 : 2, type_number);
			f           = builder{f}.emit_before<unreachable>(f);  // <-- TODO: trait block.

			while (f != f->parent->back())
				f->parent->back()->erase();
			proc->del_jump(f->parent, f->parent->successors.back());

			y->update();
			y->for_each_user([](insn* i, size_t) {
				i->update();
				return false;
			});
			return false;
		});
		proc->validate();
	}
	static void specialize_dup(procedure* proc) {
		proc->bfs([&](basic_block* bb) {
			auto i = range::find_if(bb->insns(), [](insn* i) {
				if (i->is<vdup>()) {
					i->update();
					if (i->vt == type::unk) {
						return true;
					}
				}
				return false;
			});
			if (i == bb->end()) {
				return false;
			}

			ref<> op        = i.at->operands[0];
			auto [arr, e0] = split_by(i.at, 0, type_array);
			auto [tbl, e1] = split_by(e0, 0, type_table);
			auto [fn, e2]  = split_by(e1, 0, type_function);
			// Simple move.
			e2->replace_all_uses(op);
			e2->erase();
			return false;
		});
		proc->validate();
	}
	static void specialize_len(procedure* proc) {
		proc->bfs([&](basic_block* bb) {
			auto i = range::find_if(bb->insns(), [](insn* i) {
				if (i->is<vlen>()) {
					i->update();
					if (i->operands[0]->vt == type::unk) {
						return true;
					}
				}
				return false;
			});
			if (i == bb->end()) {
				return false;
			}

			auto [arr, e0] = split_by(i.at, 0, type_array);
			auto [tbl, e1] = split_by(e0, 0, type_table);
			auto [str, e2] = split_by(e1, 0, type_string);
			// TODO: ^has trait -> e2 (would also include userdata)

			e2 = builder{e2}.emit_before<unreachable>(e2);  // <-- TODO: throw.
			while (e2 != e2->parent->back())
				e2->parent->back()->erase();
			proc->del_jump(e2->parent, e2->parent->successors.back());
			return false;
		});
		proc->validate();
	}
	static void specialize_call(procedure* proc) {
		proc->bfs([&](basic_block* bb) {
			auto i = range::find_if(bb->insns(), [](insn* i) {
				if (i->is<vcall>()) {
					i->update();
					if (i->operands[1]->vt == type::unk) {
						return true;
					}
				}
				return false;
			});
			if (i == bb->end()) {
				return false;
			}

			auto [arr, e0] = split_by(i.at, 1, type_function);
			// TODO: ^has trait -> e0
			e0 = builder{e0}.emit_before<unreachable>(e0);  // <-- TODO: throw.
			while (e0 != e0->parent->back())
				e0->parent->back()->erase();
			proc->del_jump(e0->parent, e0->parent->successors.back());
			return false;
		});
		proc->validate();
	}

	static void specialize_field(procedure* proc) {
		proc->bfs([&](basic_block* bb) {
			auto i = range::find_if(bb->insns(), [](insn* i) {
				if (i->is<field_get>() || i->is<field_set>()) {
					i->update();
					if (i->operands[1]->vt == type::unk) {
						return true;
					}
				}
				return false;
			});
			if (i == bb->end()) {
				return false;
			}

			if (i->is<field_get>()) {
				// Valid types: Table, Array, Userdata, String.
				//
				auto [tbl, e0] = split_by(i.at, 1, type_table);
				auto [arr, e1] = split_by(e0, 1, type_array);
				auto [udt, e2] = split_by(e1, 1, type_userdata);
				auto [str, e3] = split_by(e2, 1, type_string);

				arr->operands[0] = launder_value(proc, true);
				str->operands[0] = launder_value(proc, true);

				e3 = builder{e3}.emit_before<unreachable>(e3);  // <-- TODO: throw.
				while (e3 != e3->parent->back())
					e3->parent->back()->erase();
				proc->del_jump(e3->parent, e3->parent->successors.back());
			} else {
				// Valid types: Table, Array, Userdata.
				//
				auto [tbl, e0] = split_by(i.at, 1, type_table);
				auto [arr, e1] = split_by(e0, 1, type_array);
				auto [udt, e2] = split_by(e1, 1, type_userdata);

				arr->operands[0] = launder_value(proc, true);

				e2 = builder{e2}.emit_before<unreachable>(e2);  // <-- TODO: throw.
				while (e2 != e2->parent->back())
					e2->parent->back()->erase();
				proc->del_jump(e2->parent, e2->parent->successors.back());
			}

			return false;
		});
		proc->validate();
	}

	// Adds the branches for required type checks.
	//
	void type_split_cfg(procedure* proc) {
		specialize_op(proc);
		specialize_dup(proc);
		specialize_len(proc);
		specialize_call(proc);
		specialize_field(proc);
	}

	// Infers constant type information and optimizes the control flow.
	//
	void type_inference(procedure* proc) {
		bool changed = false;
		for (auto& b : proc->basic_blocks) {
			for (auto* i : b->insns()) {
				if (i->is<test_traitful>()) {
					auto resolved = get_dominating_type_at(i, i->operands[0]);
					if (resolved) {
						i->replace_all_uses(launder_value(proc, is_type_traitful(*resolved)));
						changed = true;
					}
				} else if (i->is<test_trait>()) {
					auto resolved = get_dominating_type_at(i, i->operands[0]);
					if (resolved && !is_type_traitful(*resolved)) {
						i->replace_all_uses(launder_value(proc, false));
						changed = true;
					}
				} else if (i->is<test_type>()) {
					auto expected = i->operands[1]->as<constant>()->vmtype;
					auto resolved = get_dominating_type_at(i, i->operands[0]);
					if (resolved) {
						i->replace_all_uses(launder_value(proc, *resolved == expected));
						changed = true;
					}
				}
			}
		}

		// If we changed anything, run DCE and CFG again.
		//
		if (changed) {
			dce(proc);
			cfg(proc);
		}
	}
};