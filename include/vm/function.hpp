#pragma once
#include <atomic>
#include <lang/typespec.hpp>
#include <memory>
#include <mutex>
#include <span>
#include <system_error>
#include <util/code.hpp>
#include <util/format.hpp>
#include <utility>
#include <vm/bc.hpp>
#include <vm/gc.hpp>
#include <vm/inline_cache.hpp>
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
	_(c_takes_vm)   /*True if function should be called with a VM pointer.*/

	enum functrion_attributes : uint32_t {
#define ENUM_AS_ID(x)   LI_STRCAT(func_attr_index_, x),
#define ENUM_AS_FLAG(x) LI_STRCAT(func_attr_, x) = 1u << LI_STRCAT(func_attr_index_, x),
		LIGHTNING_ENUM_ATTR(ENUM_AS_ID) LIGHTNING_ENUM_ATTR(ENUM_AS_FLAG)
#undef ENUM_AS_FLAG
#undef ENUM_AS_ID

			 func_attr_none = 0,
		func_attr_default  = func_attr_sideeffect,
	};
	inline constexpr const char* func_attr_names[] = {
#define ENUM_AS_NAME(x) LI_STRINGIFY(x),
		 LIGHTNING_ENUM_ATTR(ENUM_AS_NAME)
#undef ENUM_AS_NAME
	};

	// JIT code metadata. Executable bytes live in a separately protected mapping.
	//
	struct jfunction : gc::leaf<jfunction, type_gc_jfunc> {
		struct layout {
			size_t   code_size;
			size_t   native_stack_bytes;
			size_t   vm_stack_slots;
			uint32_t osr_target = bc::no_pos;  // Loop header this code can enter mid-invocation.
		};

		static jfunction* create(vm* L, function_proto* owner, platform::code_memory&& memory, layout code_layout, std::span<const any> constants,
			 std::unique_ptr<inline_cache_array> inline_caches);

		jfunction(platform::code_memory&& memory, layout code_layout, std::unique_ptr<inline_cache_array> caches, msize_t num_references) noexcept
			 : inline_caches(std::move(caches)),
				memory(std::move(memory)),
				code_length(code_layout.code_size),
				native_stack_requirement(code_layout.native_stack_bytes),
				vm_stack_requirement(code_layout.vm_stack_slots),
				osr_entry_target(code_layout.osr_target),
				num_references(num_references) {}
		~jfunction() noexcept;

		[[nodiscard]] const void*                entry_address() const noexcept { return memory.published() ? memory.data() : nullptr; }
		[[nodiscard]] std::span<const std::byte> code_bytes() const noexcept {
			const auto* address = static_cast<const std::byte*>(entry_address());
			return address ? std::span<const std::byte>{address, code_length} : std::span<const std::byte>{};
		}
		[[nodiscard]] std::span<any>          references() noexcept { return {reference_array, num_references}; }
		[[nodiscard]] size_t                  native_stack_bytes() const noexcept { return native_stack_requirement; }
		[[nodiscard]] size_t                  vm_stack_slots() const noexcept { return vm_stack_requirement; }
		[[nodiscard]] uint32_t                osr_target() const noexcept { return osr_entry_target; }
		[[nodiscard]] inline_cache_statistics inline_cache_stats() const { return inline_caches ? inline_caches->statistics() : inline_cache_statistics{}; }

		// Invocation publication and code mutation serialize through this lock. The returned
		// entry remains active until end_invoke, including across a suspended native frame.
		[[nodiscard]] const void*     begin_invoke() noexcept;
		void                          end_invoke() noexcept;
		[[nodiscard]] std::error_code publish_breakpoint() noexcept;

		uint32_t uid = 0;  // VTune method id.

	  private:
		std::mutex                          publication_lock;
		std::unique_ptr<inline_cache_array> inline_caches;
		platform::code_memory               memory;
		size_t                              code_length              = 0;
		size_t                              native_stack_requirement = 0;
		size_t                              vm_stack_requirement     = 0;
		uint32_t                            osr_entry_target         = bc::no_pos;
		msize_t                             num_references           = 0;
		uint32_t                            active_invocations       = 0;
		alignas(any) any reference_array[];
	};

	any_t LI_CC jit_dispatch(vm* L, any* args, slot_t n_args);

	// Parser-owned static signature copied into a prototype. Keeping the record
	// out of the GC payload avoids coupling prototype layout to typespec internals.
	//
	struct strict_signature {
		std::vector<strict::typespec> parameters;
		strict::typespec              return_type;
	};

	// VM function prototype.
	//
	struct line_info {
		msize_t ip : 18         = 0;
		msize_t line_delta : 14 = 0;
	};
	struct function_proto : gc::node<function_proto, type_gc_proto> {
		static function_proto* create(vm* L, std::span<const bc::insn> opcodes, std::span<const any> kval, std::span<const line_info> lines);

		static constexpr size_t align_payload(size_t offset, size_t alignment) { return (offset + alignment - 1) & ~(alignment - 1); }
		static constexpr size_t kval_offset(msize_t length) { return align_payload(sizeof(bc::insn) * length, alignof(any)); }
		static constexpr size_t line_offset(msize_t length, msize_t num_kval) {
			return align_payload(kval_offset(length) + sizeof(any) * num_kval, alignof(line_info));
		}
		static constexpr size_t payload_size(msize_t length, msize_t num_kval, msize_t num_lines) {
			return line_offset(length, num_kval) + sizeof(line_info) * num_lines;
		}

		uint32_t                          attr                  = func_attr_default;  // Function attributes.
		msize_t                           length                = 0;                  // Bytecode length.
		msize_t                           num_locals            = 0;                  // Number of local variables we need to reserve on stack.
		msize_t                           num_kval              = 0;                  // Number of constants.
		msize_t                           num_lines             = 0;                  // Number of lines in the line tab
		msize_t                           num_uval              = 0;                  // Number of upvalues.le.
		msize_t                           src_line              = 0;                  // Line of definition.
		mutable uint32_t                  tier_hotness          = 0;                  // Packed call/backedge counters and automatic compilation state.
		uint64_t                          return_class_identity = 0;                  // Non-owning nominal return guard for the enclosing class.
		string*                           src_chunk             = nullptr;            // Source of definition (chunk:function_name or chunk).
		string*                           jit_rejection         = nullptr;            // Last failed native-compilation diagnostic.
		table*                            attributes            = nullptr;            // Immutable source attributes, if any.
		std::unique_ptr<strict_signature> signature;                                  // Owned strict parameter/return metadata.
		std::shared_ptr<generic_template> generic;                                    // Parser-owned generic declaration and instantiation cache.
		mutable jfunction*                jfunc      = nullptr;                       // JIT function if there is one.
		mutable uint32_t                  jit_guards = 0;                             // Type guards left after optimization of the last compile.
		alignas(any) bc::insn opcode_array[];
		// any constant_array[];
		// line_table[] line_array[];

		// Range observers.
		//
		std::span<bc::insn> opcodes() { return {opcode_array, length}; }
		std::span<any>      kvals() {
			auto* payload = reinterpret_cast<std::byte*>(opcode_array);
			return {reinterpret_cast<any*>(payload + kval_offset(length)), num_kval};
		}
		std::span<line_info> lines() {
			auto* payload = reinterpret_cast<std::byte*>(opcode_array);
			return {reinterpret_cast<line_info*>(payload + line_offset(length, num_kval)), num_lines};
		}

		// Shared prototypes publish native code once. Private prototypes retain the
		// ordinary non-atomic access used by their owning VM.
		[[nodiscard]] jfunction* load_jfunc() const noexcept {
			if (!shared)
				return jfunc;
			return std::atomic_ref{jfunc}.load(std::memory_order_acquire);
		}
		void publish_jfunc(jfunction* code) noexcept {
			if (!shared) {
				jfunc = code;
				return;
			}
			std::atomic_ref{jfunc}.store(code, std::memory_order_release);
		}

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
	// Target-independent lowering hints; the native signature remains the ABI authority.
	enum class intrinsic {
		none,
		sqrt,
		abs,
		floor,
		ceil,
		trunc,
		round,
		min,
		max,
		copysign,
		cycles,
		crc32,
		array_len,
		string_len,
		typed_len,
	};
	struct nfunc_overload {
		const void*       cfunc        = nullptr;    // C function pointer. Must be LI_CC, invalid entry if nullptr.
		std::vector<type> args         = {};         // Expected argument types for a valid call.
		type              ret          = type::nil;  // Return type, if not type::any/type::exc, automatically no-except.
		intrinsic         intrinsic_id = intrinsic::none;
	};
	struct nfunc_info {
		// Function attributes.
		//
		uint32_t attr = func_attr_default;

		// Friendly name.
		//
		const char* name = nullptr;

		// Typed native overloads.
		//
		std::array<nfunc_overload, 6> overloads = {};

		// Span conversion.
		//
		std::span<const nfunc_overload> get_overloads() const {
			auto it = std::find_if(overloads.data(), overloads.data() + overloads.size(), [](auto& o) { return o.cfunc == nullptr; });
			return {overloads.data(), it};
		}
	};

	enum function_execution_flag : uint32_t {
		function_execution_jit_suppressed = 1u << 0,
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
		nfunc_t           invoke          = nullptr;  // Common function type for all calls.
		msize_t           num_uval        = 0;        // Number of upvalues.
		uint32_t          execution_flags = 0;        // Per-value runtime policy; occupies pointer-alignment padding.
		function_proto*   proto           = nullptr;  // Function prototype (if VM).
		const nfunc_info* ninfo           = nullptr;  // Native function information.
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
		bool is_jit() const { return is_virtual() && !(execution_flags & function_execution_jit_suppressed) && proto->load_jfunc() != nullptr; }

		// Duplicates the function.
		//
		function* duplicate(vm* L, bool force = false) const;

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
				puts(LI_RED "Can't dump native function.\n" LI_DEF "-------------------------------------------------------");
			}
		}
	};
};