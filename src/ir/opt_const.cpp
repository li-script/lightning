#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>
#include <lang/operator.hpp>

namespace li::ir::opt {
	// Folds constants.
	//
	void fold_constant(procedure* proc) {
		for (auto& bb : proc->basic_blocks) {
			bb->erase_if([&](insn* ins) {
				if (ins->is<select>() && ins->operands[0]->is<constant>()) {
					ins->replace_all_uses(ins->operands[0]->as<constant>()->i1 ? ins->operands[1] : ins->operands[2]);
					return true;
				}
				if (ins->is<compare>() || ins->is<binop>()) {
					if (ins->operands[1]->is<constant>() && ins->operands[2]->is<constant>()) {
						auto val = apply_binary(proc->L, ins->operands[1]->as<constant>()->to_any(), ins->operands[2]->as<constant>()->to_any(), ins->operands[0]->as<constant>()->vmopr);
						if (!val.is_exc()) {
							ins->replace_all_uses(proc->add_const(val));
							return true;
						}
					}
				}
				if (ins->is<compare>()) {
					bool is_tag_cmp = false;
					is_tag_cmp      = is_tag_cmp || (ins->operands[1]->vt == type::nil && ins->operands[2]->vt != type::any);
					is_tag_cmp      = is_tag_cmp || (ins->operands[1]->vt == type::exc && ins->operands[2]->vt != type::any);
					is_tag_cmp      = is_tag_cmp || (ins->operands[2]->vt == type::nil && ins->operands[1]->vt != type::any);
					is_tag_cmp      = is_tag_cmp || (ins->operands[2]->vt == type::exc && ins->operands[1]->vt != type::any);
					if (is_tag_cmp && ins->operands[1]->vt != ins->operands[2]->vt) {
						auto op = ins->operands[0]->as<constant>()->vmopr;
						if (op == bc::CEQ || op == bc::CNE) {
							ins->replace_all_uses(proc->add_const(op == bc::CNE));
							return true;
						}
					}
				}
				return false;
			});
		}
	}
};