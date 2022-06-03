#include <vm/function.hpp>
#include <vm/string.hpp>

namespace li {
	function* function::create(vm* L, std::span<const bc::insn> opcodes, std::span<const any> kval, size_t uval, std::span<const line_info> lines) {
		uint32_t routine_length = (uint32_t) opcodes.size();
		LI_ASSERT(routine_length != 0);

		uint32_t kval_n         = (uint32_t) kval.size();

		// Set function details.
		//
		function* result        = L->alloc<function>(sizeof(bc::insn) * routine_length + sizeof(any) * (uval + kval_n) + sizeof(line_info) * lines.size());
		result->num_uval        = (uint32_t) uval;
		result->num_kval        = kval_n;
		result->length          = routine_length;
		result->src_chunk       = string::create(L);
		result->environment     = L->globals;
		result->num_lines       = (uint32_t) lines.size();

		// Copy the information, initialize all upvalues to none.
		//
		std::copy_n(opcodes.data(), opcodes.size(), result->opcode_array);
		std::copy_n(kval.data(), kval.size(), result->kvals().begin());
		fill_none(result->uvals().data(), result->uvals().size());
		std::copy_n(lines.data(), lines.size(), result->lines().begin());
		return result;
	}

	// GC enumerator.
	//
	void gc::traverse(gc::stage_context s, function* o) {
		o->src_chunk->gc_tick(s);

		auto gc = o->gcvals();
		traverse_n(s, gc.data(), gc.size());
	}

	// Replication of vm::call.
	//
	bool nfunction::call(vm* L, slot_t n_args, slot_t caller_frame, uint32_t caller_pc) {
		LI_ASSERT(callback != nullptr);

		// Swap previous c-frame.
		//
		call_frame prevframe = L->last_vm_caller;
		if (!(caller_pc & FRAME_C_FLAG)) {
			L->last_vm_caller = call_frame{ .stack_pos = caller_frame, .caller_pc = caller_pc, .n_args = n_args };
 		}

		// Invoke callback.
		//
		auto lim = L->stack_top;
		bool ok  = callback(L, &lim[-1 - FRAME_SIZE], n_args);

		// If anything pushed, move to result slot, else set sensable defaults.
		//
		if (L->stack_top > lim) {
			lim[FRAME_RET] = L->stack_top[-1];
		} else {
			lim[FRAME_RET] = ok ? any() : L->empty_string;
		}

		// Restore c-frame and stack position, return.
		//
		L->stack_top      = lim;
		L->last_vm_caller = prevframe;
		return ok;
	}
};