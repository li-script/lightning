#pragma once
#include <span>
#include <util/format.hpp>
#include <vm/bc.hpp>
#include <vm/gc.hpp>
#include <vm/state.hpp>

namespace li {
	// Function attributes.
	//
#define LIGHTNING_ENUM_ATTR(_)                                                           \
                                                                                         \
	_(pure)         /*True if function is pure, same definition as in ir::insn.*/         \
	_(const)        /*True if function is const, same definition as in ir::insn.*/        \
	_(sideeffect)   /*True if function has sideeffects, same definition as in ir::insn.*/ \
	_(inline)       /*True if we should try inlining more aggressively.*/                 \
	_(c_takes_self) /*True if first value in args array is describing the self.*/         \
	_(c_takes_vm)   /*True if function should be called with a VM pointer.*/\

	enum functrion_attributes : uint32_t {
		#define ENUM_AS_ID(x)   LI_STRCAT(func_attr_index_, x),
		#define ENUM_AS_FLAG(x) LI_STRCAT(func_attr_, x) = 1u << LI_STRCAT(func_attr_index_, x),
		LIGHTNING_ENUM_ATTR(ENUM_AS_ID)
		LIGHTNING_ENUM_ATTR(ENUM_AS_FLAG)
		#undef ENUM_AS_FLAG
		#undef ENUM_AS_ID

		func_attr_none = 0,
		func_attr_default = func_attr_sideeffect,
	};
	inline constexpr const char* func_attr_names[] = {
		#define ENUM_AS_NAME(x) LI_STRINGIFY(x),
		LIGHTNING_ENUM_ATTR(ENUM_AS_NAME)
		#undef ENUM_AS_NAME
	};

	// JIT code.
	//
	struct jfunction : gc::exec_leaf<jfunction, type_gc_jfunc> {
		uint32_t uid;   // For VTune.
		uint32_t rsvd;  // Alignment to 16-bytes.

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
	struct function_proto : gc::node<function_proto, type_gc_proto> {
		static function_proto* create(vm* L, std::span<const bc::insn> opcodes, std::span<const any> kval, std::span<const line_info> lines);

		uint32_t   attr          = func_attr_default;  // Function attributes.
		msize_t    length        = 0;                  // Bytecode length.
		msize_t    num_locals    = 0;                  // Number of local variables we need to reserve on stack.
		msize_t    num_kval      = 0;                  // Number of constants.
		msize_t    num_lines     = 0;                  // Number of lines in the line tab
		msize_t    num_uval      = 0;                  // Number of upvalues.le.
		msize_t    src_line      = 0;                  // Line of definition.
		string*    src_chunk     = nullptr;            // Source of definition (chunk:function_name or chunk).
		jfunction* jfunc         = nullptr;            // JIT function if there is one.
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
	namespace ir {
		struct mblock;
		struct insn;
	};
	using fn_ir2mir = bool(*)(ir::mblock& b, ir::insn* i);
	struct nfunc_overload {
		const void*       cfunc = nullptr;    // C function pointer. Must be LI_CC, invalid entry if nullptr.
		std::vector<type> args  = {};         // Expected argument types for a valid call.
		type              ret   = type::nil;  // Return type, if not type::any/type::exc, automatically no-except.

		// TODO: BC->IR Lifter?
		// MIR lifter.
		//
		fn_ir2mir mir_lifter = nullptr;
	};
	struct nfunc_info {
		// Function attributes.
		//
		uint32_t attr = func_attr_default;

		// Friendly name.
		//
		const char* name = nullptr;

		// Overloads with specific lifters.
		//
		std::array<nfunc_overload, 6> overloads = {};

		// Span conversion.
		//
		std::span<const nfunc_overload> get_overloads() const {
			auto it = std::find_if(overloads.data(), overloads.data() + overloads.size(), [](auto& o) { return o.cfunc == nullptr; });
			return {overloads.data(), it};
		}
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

		// Function details.
		//
		nfunc_t           invoke   = nullptr;  // Common function type for all calls.
		msize_t           num_uval = 0;        // Number of upvalues.
		function_proto*   proto    = nullptr;  // Function prototype (if VM).
		const nfunc_info* ninfo    = nullptr;  // Native function information.
		// any               upvalue_array[];

		// TODO: Fast function alternative with types for JIT.
		//

		// Range observers.
		//
		std::span<any> uvals() { return {(any*) (this + 1), num_uval}; }

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