#include <vm/function.hpp>
#include <vm/string.hpp>

namespace lightning::core {
	function* function::create(vm* L, std::span<const bc::insn> opcodes, std::span<const any> kval, uint32_t uval) {
		uint32_t routine_length = std::max((uint32_t) opcodes.size(), 1u);
		uint32_t kval_n         = (uint32_t) kval.size();

		// Set function details.
		//
		function* result        = L->alloc<function>(sizeof(bc::insn) * routine_length + sizeof(any) * (uval + kval_n));
		result->num_uval        = uval;
		result->num_kval        = kval_n;
		result->length          = routine_length;
		result->src_chunk       = string::create(L);
		result->opcode_array[0] = bc::insn{bc::BP};
		result->environment     = L->globals;

		// Copy bytecode and constants, <none> initialize upvalues.
		//
		std::copy_n(opcodes.data(), opcodes.size(), result->opcode_array);
		std::copy_n(kval.data(), kval.size(), result->kvals().begin());
		memset(result->uvals().data(), 0xFF, result->uvals().size_bytes());
		return result;
	}

	// Duplicates the function.
	//
	function* function::duplicate(vm* L) {
		size_t    obj_len = this->object_bytes();
		size_t    ext_len = this->extra_bytes();
		function* f       = L->alloc<function>(ext_len);
		memcpy(&f->num_uval, &num_uval, obj_len);
		return f;
	}
	
	// Replication of vm::call.
	//
	bool nfunction::call(vm* L, uint32_t callsite, uint32_t n_args) {
		LI_ASSERT(callback != nullptr);

		// Save stack pos, call the handler.
		//
		uint32_t lim = L->stack_top;
		bool ok = callback(L, &L->stack[callsite+1], n_args);

		// If anything pushed, move to result slot, else set sensable defaults.
		//
		if (L->stack_top >= lim) {
			L->stack[callsite] = L->stack[lim];
		} else {
			L->stack[callsite] = ok ? any() : L->empty_string; 
		}

		// Reset stack pos and return status.
		//
		L->stack_top = lim;
		return ok;
	}
};