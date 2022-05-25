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
		size_t len = this->object_bytes(); 
		function* f = L->alloc<function>(len - sizeof(function));
		memcpy(&f->num_uval, &num_uval, len);
		return f;
	}
};