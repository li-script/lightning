#include <vm/function.hpp>
#include <vm/string.hpp>

namespace li {
	function* function::create(vm* L, std::span<const bc::insn> opcodes, std::span<const any> kval, size_t uval) {
		uint32_t routine_length = std::max((uint32_t) opcodes.size(), 1u);
		uint32_t kval_n         = (uint32_t) kval.size();

		// Set function details.
		//
		function* result        = L->alloc<function>(sizeof(bc::insn) * routine_length + sizeof(any) * (uval + kval_n));
		result->num_uval        = (uint32_t) uval;
		result->num_kval        = kval_n;
		result->length          = routine_length;
		result->src_chunk       = string::create(L);
		result->opcode_array[0] = bc::insn{bc::BP};
		result->environment     = L->globals;

		// Copy bytecode and constants, <none> initialize upvalues.
		//
		std::copy_n(opcodes.data(), opcodes.size(), result->opcode_array);
		std::copy_n(kval.data(), kval.size(), result->kvals().begin());
		fill_none(result->uvals().data(), result->uvals().size());
		return result;
	}

	// GC enumerator.
	//
	void function::gc_traverse(gc::stage_context s) {
		src_chunk->gc_tick(s);
		for (auto& v : gcvals()) {
			if (v.is_gc())
				v.as_gc()->gc_tick(s);
		}
	}

	// Duplicates the function.
	//
	function* function::duplicate(vm* L) {
		return L->duplicate<function>(this);
	}
	
	// Replication of vm::call.
	//
	bool nfunction::call(vm* L, slot_t n_args, slot_t caller_frame, uint32_t caller_pc) {
		LI_ASSERT(callback != nullptr);

		// Swap previous c-frame.
		//
		slot_t prevcframe = std::exchange(L->last_vm_caller, caller_pc != FRAME_C_IP ? caller_frame : L->last_vm_caller);

		// Invoke callback.
		//
		slot_t lim = L->stack_top;
		bool   ok  = callback(L, &L->stack[L->stack_top - 1 - FRAME_SIZE], n_args);

		// If anything pushed, move to result slot, else set sensable defaults.
		//
		if (L->stack_top > lim) {
			L->stack[lim + FRAME_RET] = L->stack[L->stack_top - 1];
		} else {
			L->stack[lim + FRAME_RET] = ok ? any() : L->empty_string;
		}

		// Restore c-frame and stack position, return.
		//
		L->stack_top      = lim;
		L->last_vm_caller = prevcframe;
		return ok;
	}
};