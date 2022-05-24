#pragma once
#include <span>
#include <vm/gc.hpp>
#include <vm/bc.hpp>
#include <vm/state.hpp>

namespace lightning::core {
	struct function : gc_node<function> {
		static function* create(vm* L, std::span<const bc::insn> opcodes, std::span<const any> kval, uint32_t uval);

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

		// GC enumerator.
		//
		template<typename F>
		void enum_for_gc(F&& fn) {
			fn(src_chunk);
			for (auto& v : gcvals()) {
				if (v.is_gc())
					fn(v.as_gc());
			}
		}
	};
};