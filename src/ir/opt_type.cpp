#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>

namespace li::ir::opt {
	// Infers constant type information and optimizes the control flow.
	//
	void type_inference(procedure* proc) {
		bool changed = false;
		for (auto& b : proc->basic_blocks) {
			for (auto* i : b->insns()) {
				if (!i->is<test_type>())
					continue;
				auto expected = i->operands[1]->as<constant>()->vmtype;

				if (i->operands[0]->vt != type::unk) {
					constant test;
					test.vt = i->operands[0]->vt;
					i->replace_all_uses(launder_value(proc, test.to_any().type() == expected));
					changed = true;
					continue;
				}

				std::optional<value_type> resolved;
				i->operands[0]->as<insn>()->for_each_user([&](insn* c, size_t j) {
					if (c->is<try_cast>() || c->is<assume_cast>()) {
						if (c->parent->dom(b.get())) {
							resolved = i->operands[1]->as<constant>()->vmtype;
							return true;
						}
					}
					return false;
				});
				if (resolved) {
					i->replace_all_uses(launder_value(proc, *resolved == expected));
					changed = true;
					continue;
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