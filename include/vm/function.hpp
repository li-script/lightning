#pragma once
#include <span>
#include <vm/bc.hpp>
#include <vm/gc.hpp>
#include <vm/state.hpp>

namespace li {
	// Native callback, at most one result should be pushed on stack, if returns false, signals exception.
	//
	using nfunc_t = bool (*)(vm* L, any* args, slot_t n);

	// nfunc_t for virtual functions.
	//
	bool vm_invoke(vm* L, any* args, slot_t n_args);

	// JIT code.
	//
	struct jfunction : gc::exec_leaf<jfunction> {
		// For 16-byte alignment.
		//
		uint64_t rsvd;

		// Generated code.
		//
		uint8_t code[];
	};

	// VM function.
	//
	struct line_info {
		msize_t ip : 18         = 0;
		msize_t line_delta : 14 = 0;
	};
	struct function_proto : gc::node<function_proto, type_proto> {
		static function_proto* create(vm* L, std::span<const bc::insn> opcodes, std::span<const any> kval, std::span<const line_info> lines);

		msize_t    length        = 0;        // Bytecode length.
		msize_t    num_locals    = 0;        // Number of local variables we need to reserve on stack.
		msize_t    num_kval      = 0;        // Number of constants.
		msize_t    num_lines     = 0;        // Number of lines in the line tab
		msize_t    num_arguments = 0;        // Number of fixed arguments.
		msize_t    num_uval      = 0;        // Number of upvalues.le.
		msize_t    src_line      = 0;        // Line of definition.
		string*    src_chunk     = nullptr;  // Source of definition (chunk:function_name or chunk).
		jfunction* jfunc         = nullptr;  // JIT function if there is one.
		bc::insn   opcode_array[];
		// any constant_array[];
		// line_table[] line_array[];

		// Range observers.
		//
		std::span<bc::insn>  opcodes() { return {opcode_array, length}; }
		std::span<any>       kvals() { return {(any*) &opcode_array[length], num_kval}; }
		std::span<line_info> lines() { return {(line_info*) (num_kval + (any*) &opcode_array[length]), num_lines}; }

		// Converts BC -> Line.
		//
		msize_t lookup_line(bc::pos pos) {
			msize_t n = src_line;
			for (auto [ip, delta] : lines()) {
				if (ip >= pos)
					break;
				n += delta;
			}
			return n;
		}
	};
	struct function : gc::node<function, type_function> {
		// Creates a new instance given the prototype.
		//
		static function* create(vm* L, function_proto* proto);

		// Creates a simple native function.
		//
		static function* create(vm* L, nfunc_t cb);

		// Function details.
		//
		nfunc_t         invoke            = nullptr;  // Common function type for all calls.
		msize_t         num_arguments : 6 = 0;        // Number of fixed arguments.
		msize_t         num_uval : 26     = 0;        // Number of upvalues.
		table*          environment       = nullptr;  // Table environment.
		function_proto* proto             = nullptr;  // Function prototype (if VM).
		any             upvalue_array[];

		// TODO: Fast function alternative with types for JIT.
		//

		// Range observers.
		//
		std::span<any> uvals() { return {(any*) upvalue_array, num_uval}; }

		// Checks for the function type.
		//
		bool is_native() const { return proto == nullptr; }
		bool is_virtual() const { return proto != nullptr; }
		bool is_jit() const { return is_virtual() && invoke != vm_invoke; }

		// Duplicates the function.
		//
		function* duplicate(vm* L, bool force = false) const {
			// Function has no state, no need to copy anything.
			//
			if (!num_uval && !force)
				return (function*) this;
			else
				return L->duplicate(this);
		}
	};
};