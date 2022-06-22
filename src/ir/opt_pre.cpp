#include <util/common.hpp>
#if LI_JIT

#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>
#include <lib/std.hpp>
#include <vm/runtime.hpp>
#include <vm/array.hpp>
#include <vm/table.hpp>
#include <vm/function.hpp>
#include <cmath>
#include <numbers>

namespace li::ir::opt {
	// Prepares the IR to be lifted to MIR.
	//
	void prepare_for_mir(procedure* proc) {
		// Helper to replace an instruction with call.
		//
		auto replace_with_call = [&](instruction_iterator i, const nfunc_info* nf, int32_t oidx, auto&&... args) {
			auto nit = builder{}.emit_after<ccall>(i.at, nf, oidx, args...);
			i->replace_all_uses(nit.at);
			i->erase();
			return instruction_iterator(nit);
		};

		for (auto& bb : proc->basic_blocks) {
			for (auto it = bb->begin(); it != bb->end();) {

				// Array/Table new.
				//
				if (it->is<array_new>()) {
					it = replace_with_call(it, &lib::detail::builtin_new_array.nfi, 0, it->operands[0]);
					continue;
				} else if (it->is<table_new>()) {
					it = replace_with_call(it, &lib::detail::builtin_new_table.nfi, 0, it->operands[0]);
					continue;
				}

				// Mod and pow.
				//
#if !LI_FAST_MATH
				if (it->is<binop>() && it->operands[0]->as<constant>()->vmopr == bc::AMOD) {
					switch (it->vt) {
						case type::f64:
							it = replace_with_call(it, &lib::detail::math_mod_info, 0, it->operands[1], it->operands[2]);
							break;
						default:
							util::abort("unexpected AMOD with invalid or unknown type.");
					}
					continue;
				}
#endif
				if (it->is<binop>() && it->operands[0]->as<constant>()->vmopr == bc::APOW) {
					// TODO: Prob better if we moved it out of here?
					switch (it->vt) {
						case type::f64:
							if (it->operands[1]->is<constant>()) {
								double n = it->operands[1]->as<constant>()->n;
								if (n == 2) {
									it = replace_with_call(it, &lib::detail::math_exp2.nfi, 0, it->operands[2]);
									break;
								} else if (n == std::numbers::e) {
									it = replace_with_call(it, &lib::detail::math_exp.nfi, 0, it->operands[2]);
									break;
								}
							}
							if (it->operands[2]->is<constant>()) {
								double n = it->operands[2]->as<constant>()->n;
								if (n == (1.0 / 2)) {
									it = replace_with_call(it, &lib::detail::math_sqrt.nfi, 0, it->operands[1]);
									break;
								} else if (n == (1.0 / 3)) {
									it = replace_with_call(it, &lib::detail::math_cbrt.nfi, 0, it->operands[1]);
									break;
								}
							}
							it = replace_with_call(it, &lib::detail::math_pow.nfi, 0, it->operands[1], it->operands[2]);
							break;
						default:
							util::abort("unexpected APOW with invalid or unknown type.");
					}
					continue;
				}
				++it;
			}
		}	
	}
	void finalize_for_mir(procedure* proc) {
		// Fixup PHIs, this should be the last operation as they are not allowed to be optimized out.
		//
		for (auto& bb : proc->basic_blocks) {
			for (auto phi : bb->insns()) {
				if (!phi->is<ir::phi>())
					break;
				for (size_t i = 0; i != phi->operands.size(); i++) {
					if (phi->vt == type::any && phi->operands[i]->vt != type::any)
						phi->operands[i] = builder{}.emit_before<erase_type>(bb->predecessors[i]->back(), phi->operands[i]);
					else
						phi->operands[i] = builder{}.emit_before<move>(bb->predecessors[i]->back(), phi->operands[i]);
				}
			}
		}

		// Topologically sort the procedure and rename every register.
		//
		proc->topological_sort();

		// Add loop hints.
		//  TODO: Not reaaaly
		//
		for (auto& blk : proc->basic_blocks) {
			for (auto& s : blk->successors) {
				if (s->uid < blk->uid) {
					for (size_t i = s->uid; i <= blk->uid; ++i) {
						proc->basic_blocks[i]->loop_depth++;
					}
				}
			}
		}

		// Reset the names and validate.
		//
		proc->reset_names();
		proc->validate();
	}
};
#endif