#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>

namespace li::ir::opt {
	// Helper to convert from IR to VM type.
	//
	static value_type ir_to_vm(type r) {
		constant test;
		test.vt = r;
		return test.to_any().type();
	}

	// Resolves dominating type for a value at the given instruction.
	//
	static std::optional<value_type> get_dominating_type_at(insn* i, value* v) {
		if (v->vt != type::unk) {
			return ir_to_vm(v->vt);
		} else {
			std::optional<value_type> resolved;
			v->as<insn>()->for_each_user([&](insn* c, size_t j) {
				// TODD: test_type branch check, has path to.
				if (c->is<assume_cast>()) {
					if (c->parent->dom(i->parent)) {
						resolved = ir_to_vm(c->operands[1]->as<constant>()->irtype);
						return true;
					}
				}
				return false;
			});
			return resolved;
		}
	}

	// Adds the branches for required type checks.
	//
	void type_split_cfg(procedure* proc) {
		// Force numeric operations to be numbers.
		//
		auto get_unreachable_block = [&proc, unreachable_block = (basic_block*) nullptr]() mutable {
			if (unreachable_block)
				return unreachable_block;
			auto* b = proc->add_block();
			builder{b}.emit_front<unreachable>();
			b->cold_hint      = 100;
			unreachable_block = b;
			return b;
		};
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

			// Insert an is number check before.
			//
			builder b{i.at};
			auto    op = i->operands[1]->vt == type::unk ? 1 : 2;
			auto    cc = b.emit_before<test_type>(i.at, i->operands[op], type_number);

			// Split the block after the check, add the jcc.
			//
			auto cont_blk   = bb->split_at(cc);
			auto num_blk    = proc->add_block();
			auto nonnum_blk = get_unreachable_block();  // proc->add_block();
			b.emit<jcc>(cc, num_blk, nonnum_blk);
			proc->add_jump(bb, num_blk);
			proc->add_jump(bb, nonnum_blk);

			// Unlink the actual operation and move it to another block.
			//
			b.blk           = num_blk;
			i->operands[op] = b.emit<assume_cast>(i->operands[op], type::f64);
			auto v1         = num_blk->push_back(i->erase());
			v1->update();
			b.emit<jmp>(cont_blk);
			proc->add_jump(num_blk, cont_blk);

			// Replace uses.
			//
			i->for_each_user_outside_block([&](insn* i, size_t op) {
				i->operands[op].reset(v1.at);
				return false;
			});

			// TODO: Trait stuff.
			// b.blk           = nonnum_blk;
			// auto v2 = i->is<compare>() ? b.emit<move>(false) : b.emit<move>(0);
			// b.emit<jmp>(cont_blk);
			// proc->add_jump(nonnum_blk, cont_blk);
			//
			//// Create a PHI at the destination.
			////
			// b.blk = cont_blk;
			// auto x = b.emit_front<phi>(v1.at, v2.at);
			//
			//// Replace uses.
			////
			// cont_blk->validate();
			// i->for_each_user_outside_block([&](insn* i, size_t op) {
			//	if (i!=x)
			//		i->operands[op].reset(x);
			//	i->parent->validate();
			//	return false;
			// });
			return false;
		});
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