#pragma once
#include <span>
#include <util/format.hpp>
#include <vm/bc.hpp>
#include <vm/gc.hpp>
#include <vm/state.hpp>

namespace li {
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

	// VM function prototype.
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

	// Native function details.
	// - Not GC allocated.
	//
	using fn_bc2ir = bool(*)(ir::builder& bld);
	struct nfunc_overload {
		const void*             cfunc      = nullptr;         // C function pointer. Must be LI_CC, invalid entry if nullptr.
		bool                    takes_self = false;           // True if first value in args array is describing the self.
		bool                    takes_vm   = false;           // True if function should be called with a VM pointer.
		ir::type                ret        = ir::type::none;  // Type of return value.
		std::array<ir::type, 8> args       = {};              // Expected argument types for a valid call, none = end.
	};
	struct nfunc_info {
		// True if function is pure / const, same definition as in ir::insn.
		//
		uint32_t is_pure : 1  = true;
		uint32_t is_const : 1 = false;

		// True if function never throws (so long as the argument types match the overload).
		//
		uint32_t no_throw : 1 = false;

		// Friendly name.
		//
		const char* name = nullptr;

		// nfunc_t callback, for convenience.
		//
		nfunc_t invoke = nullptr;

		// Overloads with specific lifters.
		//
		std::array<nfunc_overload, 5> overloads = {};
	};

	// "Type" erased function type.
	//
	struct function : gc::node<function, type_function> {
		// Creates a new instance given the prototype.
		//
		static function* create(vm* L, function_proto* proto);

		// Creates a native function.
		//
		static function* create(vm* L, nfunc_t cb);
		static function* create(vm* L, const nfunc_info* info);

		// Function details.
		//
		nfunc_t           invoke   = nullptr;  // Common function type for all calls.
		msize_t           num_uval = 0;        // Number of upvalues.
		function_proto*   proto    = nullptr;  // Function prototype (if VM).
		const nfunc_info* ninfo    = nullptr;  // Native function information.
		any               upvalue_array[];

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

		// Prints bytecode.
		//
		void print_bc() {
			if (proto) {
				puts(
					 "Dumping bytecode of the function:\n"
					 "-------------------------------------------------------");
				msize_t last_line = 0;
				for (msize_t i = 0; i != proto->length; i++) {
					if (msize_t l = proto->lookup_line(i); l != last_line) {
						last_line = l;
						printf("ln%-52u", l);
						printf("|\n");
					}
					proto->opcode_array[i].print(i);
				}
				puts("-------------------------------------------------------");
				if (num_uval) {
					for (msize_t i = 0; i != num_uval; i++) {
						printf(LI_CYN "u%u:   " LI_DEF, i);
						uvals()[i].print();
						printf("\n");
					}
					puts("-------------------------------------------------------");
				}
			} else {
				puts(
					 LI_RED "Can't dump native function.\n"
					 LI_DEF "-------------------------------------------------------");
			}
		}
	};
};