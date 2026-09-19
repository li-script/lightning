#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>
#include <lang/operator.hpp>
#include <vm/array.hpp>
#include <vm/state.hpp>

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
						any                lhs             = ins->operands[1]->as<constant>()->to_any();
						any                rhs             = ins->operands[2]->as<constant>()->to_any();
						constexpr uint32_t dynamic_effects = effect_may_call_user | effect_may_throw;
						bool               primitive_safe  = !lhs.is_gc() && !rhs.is_gc();
						if (primitive_safe || !(ins->effects & dynamic_effects)) {
							any    previous_exception = proc->L->last_ex;
							array* previous_trace     = proc->L->last_exception_trace;
							rc::retain(previous_exception);
							rc::retain(previous_trace);
							auto val = apply_binary(proc->L, lhs, rhs, ins->operands[0]->as<constant>()->vmopr);
							rc::replace_adopt(proc->L, proc->L->last_ex, previous_exception);
							array* discarded_trace        = proc->L->last_exception_trace;
							proc->L->last_exception_trace = previous_trace;
							rc::release(proc->L, discarded_trace);

							if (!val.is_exc()) {
								if (val.is_gc()) {
									// JIT constant pools do not own heap values yet.
									rc::release(proc->L, val);
								} else {
									ins->replace_all_uses(proc->add_const(val));
									return true;
								}
							}
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