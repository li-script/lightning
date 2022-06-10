#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>

namespace li::ir::opt {
	// Re-schedules GC ticks.
	//
	void schedule_gc(procedure* proc) {
		for (auto& bb : proc->basic_blocks) {
			// TODO: Clearly needs more logic, move out of loop if fields are constant and
			// scheduled because of TSET.
			//
			size_t n = bb->erase_if([&](insn* ins) {
				return ins->is<gc_tick>();
			});
			if (n) {
				builder{bb.get()}.emit_before<gc_tick>(bb->back());
			}
		}
	}
};