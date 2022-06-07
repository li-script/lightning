#include <util/common.hpp>
#if LI_JIT

#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>
#include <vm/runtime.hpp>
#include <vm/array.hpp>
#include <vm/table.hpp>
#include <vm/function.hpp>
#include <cmath>

namespace li::ir::opt {
	// Prepares the IR to be lifted to MIR.
	//
	void prepare_for_mir(procedure* proc) {
		// Helper to replace an instruction with call.
		//
		auto replace_with_call = [&] (instruction_iterator i, bool vm, type ret, auto* fn, auto&&... args) {
			auto nit = builder{}.emit_after<ccall>(i.at, vm, ret, (intptr_t) bit_cast<void*>(fn), args...);
			i->replace_all_uses(nit.at);
			i->erase();
			return instruction_iterator(nit);
		};

		for (auto& bb : proc->basic_blocks) {
			for (auto it = bb->begin(); it != bb->end();) {

				// Array/Table new.
				//
				if (it->is<array_new>()) {
					it = replace_with_call(it, true, type::arr, &runtime::array_new, it->operands[0]);
					continue;
				} else if (it->is<table_new>()) {
					it = replace_with_call(it, true, type::tbl, &runtime::table_new, it->operands[0]);
					continue;
				}

				// Duplication.
				//
				if (it->is<vdup>()) {
					switch (it->operands[0]->vt) {
						case type::arr:
							it = replace_with_call(
								 it, true, it->operands[0]->vt, +[](vm* L, array* a) { return a->duplicate(L); }, it->operands[0]);
							break;
						case type::tbl:
							it = replace_with_call(
								 it, true, it->operands[0]->vt, +[](vm* L, table* a) { return a->duplicate(L); }, it->operands[0]);
							break;
						case type::fn:
							it = replace_with_call(
								 it, true, it->operands[0]->vt, +[](vm* L, function* a) { return a->duplicate(L); }, it->operands[0]);
							break;
						default:
							util::abort("unexpected dup with invalid or unknown type.");
					}
					continue;
				}

				// Mod and pow.
				//
#if !LI_FAST_MATH
				if (it->is<binop>() && it->operands[0]->as<constant>()->vmopr == bc::AMOD) {
					switch (it->vt) {
						case type::f64:
							it = replace_with_call(
								 it, false, type::f64, +[](double x, double y) { return fmod(x, y); }, it->operands[1], it->operands[2]);
							break;
						default:
							util::abort("unexpected AMOD with invalid or unknown type.");
					}
					continue;
				}
#endif
				if (it->is<binop>() && it->operands[0]->as<constant>()->vmopr == bc::APOW) {
					switch (it->vt) {
						case type::f64:
							it = replace_with_call(
								 it, false, type::f64, +[](double x, double y) { return pow(x, y); }, it->operands[1], it->operands[2]);
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
					if (phi->vt == type::unk && phi->operands[i]->vt != type::unk)
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
		//  TODO: Not reaaaly + move to procedure.
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