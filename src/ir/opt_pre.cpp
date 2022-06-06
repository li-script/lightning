#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>
#include <vm/runtime.hpp>

namespace li::ir::opt {
	// Prepares the IR to be lifted to MIR.
	//
	void prepare_for_mir(procedure* proc) {
		// Helper to replace an instruction with call.
		//
		auto replace_with_call = [&](instruction_iterator i, bool vm, type ret, const void* fn, auto&&... args) {
			auto nit = builder{}.emit_after<ccall>(i.at, vm, ret, (int64_t) fn, args...);
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
		proc->print();
	}
};