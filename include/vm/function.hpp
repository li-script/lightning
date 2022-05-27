#pragma once
#include <span>
#include <vm/gc.hpp>
#include <vm/bc.hpp>
#include <vm/state.hpp>

namespace li {
	// Native callback, at most one result should be pushed on stack, if returns false, signals exception.
	//
	using nfunc_t = bool (*)(vm* L, const any* args, uint32_t n);

	// Native function.
	//
	struct nfunction : gc::leaf<nfunction, type_nfunction> {
		static nfunction* create(vm* L, size_t context = 0) { return L->alloc<nfunction>(context); }

		// Stack-based callback.
		//
		nfunc_t  callback      = nullptr;
		uint32_t num_arguments = 0;

		// TODO: Fast function alternative with types for JIT.
		//

		// Replication of vm::call.
		//
		bool call(vm* L, uint32_t callsite, uint32_t n_args);
	};

	// VM function.
	//
	struct function : gc::node<function, type_function> {
		static function* create(vm* L, std::span<const bc::insn> opcodes, std::span<const any> kval, size_t uval);

		// Function details.
		//
		uint32_t num_uval      = 0;        // Number of upvalues.
		uint32_t num_kval      = 0;        // Number of constants.
		uint32_t num_arguments = 0;        // Vararg if zero, else n-1 args.
		uint32_t num_locals    = 0;        // Number of local variables we need to reserve on stack.
		uint32_t length        = 0;        // Bytecode length.
		uint32_t src_line      = 0;        // Line of definition.
		string*  src_chunk     = nullptr;  // Source of definition.
		table*   environment   = nullptr;  // Table environment.

		// Variable length part.
		//
		bc::insn opcode_array[];
		// any upvalue_array[];
		// any constant_array[];

		// TODO: Debug info?

		// Range observers.
		//
		std::span<bc::insn> opcodes() { return {opcode_array, length}; }
		std::span<any>      uvals() { return {(any*) &opcode_array[length], num_uval}; }
		std::span<any>      kvals() { return {num_uval + (any*) &opcode_array[length], num_kval}; }
		std::span<any>      gcvals() { return {(any*) &opcode_array[length], num_uval + num_kval}; }

		// Duplicates the function.
		//
		function* duplicate(vm* L);

		// GC enumerator.
		//
		void gc_traverse(gc::sweep_state& s) override;
	};
};