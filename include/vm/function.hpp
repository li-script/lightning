#pragma once
#include <span>
#include <vm/bc.hpp>
#include <vm/gc.hpp>
#include <vm/state.hpp>

namespace li {
	// Native callback, at most one result should be pushed on stack, if returns false, signals exception.
	//
	using nfunc_t = bool (*)(vm* L, any* args, slot_t n);

	// JIT code.
	//
	struct jfunction : gc::exec_leaf<jfunction> {
		uint8_t code[];
	};

	// Native function.
	//
	struct nfunction : gc::leaf<nfunction, type_nfunction> {
		static nfunction* create(vm* L, size_t context = 0) { return L->alloc<nfunction>(context); }
		static nfunction* create(vm* L, nfunc_t f, size_t context = 0) {
			nfunction* nf = create(L, context);
			nf->callback  = f;
			return nf;
		}

		// Stack-based callback.
		//
		nfunc_t  callback      = nullptr;
		uint32_t num_arguments = 0;

		// TODO: Fast function alternative with types for JIT.
		//

		// TODO: Remove, just for debugging.
		//
		bool jit = false;

		// Replication of vm::call.
		//
		bool call(vm* L, slot_t n_args, slot_t caller_frame, uint32_t caller_pc);
	};

	// VM function.
	//
	struct line_info {
		uint32_t ip : 18         = 0;
		uint32_t line_delta : 14 = 0;
	};
	struct function : gc::node<function, type_function> {
		static function* create(vm* L, std::span<const bc::insn> opcodes, std::span<const any> kval, size_t uval, std::span<const line_info> lines);

		// Function details.
		//
		uint32_t num_arguments : 6 = 0;        // Number of fixed arguments.
		uint32_t num_locals : 26   = 0;        // Number of local variables we need to reserve on stack.
		uint32_t num_uval          = 0;        // Number of upvalues.
		uint32_t num_kval          = 0;        // Number of constants.
		uint32_t length            = 0;        // Bytecode length.
		uint32_t src_line          = 0;        // Line of definition.
		string*  src_chunk         = nullptr;  // Source of definition (chunk:function_name or chunk).
		uint32_t num_lines         = 0;
		table*   environment       = nullptr;  // Table environment.

		// Variable length part.
		//
		bc::insn opcode_array[];
		// any upvalue_array[];
		// any constant_array[];
		// line_table[] line_array[];

		// Range observers.
		//
		std::span<bc::insn>  opcodes() { return {opcode_array, length}; }
		std::span<any>       uvals() { return {(any*) &opcode_array[length], num_uval}; }
		std::span<any>       kvals() { return {num_uval + (any*) &opcode_array[length], num_kval}; }
		std::span<any>       gcvals() { return {(any*) &opcode_array[length], num_uval + num_kval}; }
		std::span<line_info> lines() { return {(line_info*) (num_uval + num_kval + (any*) &opcode_array[length]), num_lines}; }

		// Duplicates the function.
		//
		function* duplicate(vm* L) const { return L->duplicate(this); }

		// Converts BC -> Line.
		//
		uint32_t lookup_line(bc::pos pos) {
			uint32_t n = src_line;
			for (auto [ip, delta] : lines()) {
				if (ip >= pos)
					break;
				n += delta;
			}
			return n;
		}
	};
};