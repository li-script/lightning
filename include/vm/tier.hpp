#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vm/types.hpp>

namespace li {
	struct function;
	struct nfunc_info;
	struct vm;

	namespace tier {
		inline constexpr uint16_t default_call_threshold     = 8;
		inline constexpr uint16_t default_backedge_threshold = 64;
		inline constexpr uint16_t maximum_hot_threshold      = 0x7ffe;

		enum class mode : uint8_t {
			off,
			automatic,
			required,
		};

		struct policy {
			mode     execution          = mode::off;
			bool     verbose            = false;
			uint16_t call_threshold     = default_call_threshold;
			uint16_t backedge_threshold = default_backedge_threshold;
		};

		// Explicit enable/disable is a property of a private function value, not of
		// its potentially shared prototype. Shared function values are immutable.
		[[nodiscard]] bool enable(function* value) noexcept;
		[[nodiscard]] bool disable(function* value) noexcept;
		[[nodiscard]] bool is_enabled(const function* value) noexcept;

		// Automatic tiering compiles at a hot call entry. A hot interpreted
		// backedge may transfer the current owning frame into a generated OSR entry.
		void               on_call(vm* L, function* value);
		[[nodiscard]] bool on_backedge(vm* L, function* value, any* args, slot_t n_args, uint32_t target_pc, any_t* result);

#if LI_JIT
		// Generated-code coordination. OSR entry consumes the interpreter's local
		// owners; deoptimization rebuilds them before resuming bytecode execution.
		extern const nfunc_info osr_entry_info;
		void LI_CC              prepare_jit_frame(vm* L, any* local0, slot_t num_locals, slot_t scratch_slots);
		void LI_CC              deopt_store(vm* L, any* slot, any_t value);
		any_t LI_CC             deopt_resume(vm* L, any* args, slot_t n_args, uint64_t resume_state);
#endif

		// Loop header the in-progress compilation was requested for by a hot
		// backedge; the compiler adds an OSR entry for it.
		[[nodiscard]] std::optional<uint32_t> pending_osr_target() noexcept;

		// Required mode uses this for prototypes created after parsing. Unlike an
		// automatic attempt, rejection is returned to the invoking script.
		[[nodiscard]] std::optional<std::string> compile_required(vm* L, function* value);

		inline constexpr uint64_t pack_resume_state(uint32_t bytecode_pc, uint32_t exception_handler_pc, uint32_t cleanup_handler_pc) noexcept {
			constexpr uint64_t field_mask     = (uint64_t{1} << 19) - 1;
			auto               encode_handler = [=](uint32_t pc) { return pc == UINT32_MAX ? uint64_t{0} : (uint64_t(pc) + 1) & field_mask; };
			return uint64_t(bytecode_pc) | (encode_handler(exception_handler_pc) << 18) | (encode_handler(cleanup_handler_pc) << 37);
		}
	}
}
