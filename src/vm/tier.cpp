#include <algorithm>
#include <atomic>
#include <ir/runtime.hpp>
#include <lib/std.hpp>
#include <vm/function.hpp>
#include <vm/rc.hpp>
#include <vm/state.hpp>
#include <vm/tier.hpp>

namespace li::tier {
	namespace {
		constexpr uint32_t backedge_shift = 15;
#if LI_JIT
		struct osr_request {
			vm*             L;
			function_proto* proto;
			any*            args;
			uint32_t        target_pc;
			bool            prepared = false;
		};
		thread_local osr_request* active_osr = nullptr;
		// Loop header requested by the compilation currently running on this thread.
		thread_local uint32_t osr_compile_target = bc::no_pos;

		uint8_t LI_CC osr_entry(vm* L, int32_t bytecode_pc) {
			if (!active_osr || active_osr->L != L || !active_osr->prepared || active_osr->target_pc != uint32_t(bytecode_pc))
				return 0;
			active_osr->target_pc = bc::no_pos;
			active_osr            = nullptr;
			return 1;
		}
		constexpr uint32_t counter_mask        = 0x7fff;
		constexpr uint32_t compilation_claimed = uint32_t{1} << 30;
		constexpr uint32_t compilation_failed  = uint32_t{1} << 31;

		uint16_t bounded_threshold(uint16_t value) noexcept { return std::clamp<uint16_t>(value, 1, maximum_hot_threshold); }

		bool claim_private(uint32_t& hotness, uint32_t shift, uint16_t threshold) noexcept {
			if (hotness & compilation_claimed)
				return false;
			const uint32_t count = (hotness >> shift) & counter_mask;
			const uint32_t next  = std::min(count + 1, counter_mask);
			hotness              = (hotness & ~(counter_mask << shift)) | (next << shift);
			if (next < bounded_threshold(threshold))
				return false;
			hotness |= compilation_claimed;
			return true;
		}

		bool claim_shared(function_proto* proto, uint32_t shift, uint16_t threshold) noexcept {
			auto     hotness = std::atomic_ref{proto->tier_hotness};
			uint32_t current = hotness.load(std::memory_order_relaxed);
			while (!(current & compilation_claimed)) {
				const uint32_t count   = (current >> shift) & counter_mask;
				const uint32_t next    = std::min(count + 1, counter_mask);
				uint32_t       desired = (current & ~(counter_mask << shift)) | (next << shift);
				const bool     claim   = next >= bounded_threshold(threshold);
				if (claim)
					desired |= compilation_claimed;
				if (hotness.compare_exchange_weak(current, desired, std::memory_order_relaxed))
					return claim;
			}
			return false;
		}

		bool claim(function_proto* proto, uint32_t shift, uint16_t threshold) noexcept {
			return proto->shared ? claim_shared(proto, shift, threshold) : claim_private(proto->tier_hotness, shift, threshold);
		}

		void mark_failed(function_proto* proto) noexcept {
			if (proto->shared)
				std::atomic_ref{proto->tier_hotness}.fetch_or(compilation_failed, std::memory_order_relaxed);
			else
				proto->tier_hotness |= compilation_failed;
		}

#endif

		void record_hotness(vm* L, function* value, uint32_t shift, uint16_t threshold, uint32_t osr_target = bc::no_pos) {
#if LI_JIT
			if (!L || !value || !value->is_virtual() || L->jit_policy.execution != mode::automatic || !is_enabled(value) || value->proto->load_jfunc())
				return;
			if (!claim(value->proto, shift, threshold))
				return;
			auto previous      = std::exchange(osr_compile_target, osr_target);
			auto error         = lib::jit_on(L, value, L->jit_policy.verbose);
			osr_compile_target = previous;
			if (error)
				mark_failed(value->proto);
#else
			(void) L;
			(void) value;
			(void) shift;
			(void) threshold;
			(void) osr_target;
#endif
		}
	}

	bool enable(function* value) noexcept {
		if (!value || !value->is_virtual())
			return false;
		if (!value->shared)
			value->execution_flags &= ~function_execution_jit_suppressed;
		return true;
	}

	bool disable(function* value) noexcept {
		if (!value || !value->is_virtual() || value->shared)
			return false;
		value->execution_flags |= function_execution_jit_suppressed;
		return true;
	}

	bool is_enabled(const function* value) noexcept { return value && value->is_virtual() && !(value->execution_flags & function_execution_jit_suppressed); }

	void on_call(vm* L, function* value) { record_hotness(L, value, 0, L ? L->jit_policy.call_threshold : default_call_threshold); }

	bool on_backedge(vm* L, function* value, any* args, slot_t n_args, uint32_t target_pc, any_t* result) {
#if LI_JIT
		record_hotness(L, value, backedge_shift, L ? L->jit_policy.backedge_threshold : default_backedge_threshold, target_pc);
		if (!L || !value || !args || !result || !is_enabled(value) || active_osr || target_pc >= value->proto->length)
			return false;
		// Only code compiled for this header may adopt the frame: any other entry
		// would restart the function and replay its side effects.
		jfunction* code = value->proto->load_jfunc();
		if (!code || code->osr_target() != target_pc)
			return false;
		any* local0 = args + FRAME_SIZE + 1;
		if (L->stack_top != local0 + value->proto->num_locals)
			return false;

		osr_request request{L, value->proto, args, target_pc};
		auto*       previous = std::exchange(active_osr, &request);
		*result              = jit_dispatch(L, args, n_args);
		active_osr           = previous;
		return true;
#else
		(void) L;
		(void) value;
		(void) args;
		(void) n_args;
		(void) target_pc;
		(void) result;
		return false;
#endif
	}

#if LI_JIT
	const nfunc_info osr_entry_info = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 "tier.osr_entry",
		 {nfunc_overload{li::bit_cast<const void*>(&osr_entry), {type::i32}, type::i1}},
	};

	void LI_CC prepare_jit_frame(vm* L, any* local0, slot_t num_locals, slot_t scratch_slots) {
		any* args = local0 - (FRAME_SIZE + 1);
		if (active_osr && active_osr->L == L && active_osr->args == args && active_osr->proto && active_osr->proto->num_locals == msize_t(num_locals)) {
			if (L->stack_top != local0 + num_locals || !L->vm_stack_headroom_available(size_t(scratch_slots)))
				L->panic("invalid OSR frame");
			active_osr->prepared = true;
			return;
		}
		ir::runtime::frame_enter(L, local0, num_locals, scratch_slots);
	}

	// The deoptimizing frame consumed its operands, so the slot adopts ownership.
	void LI_CC deopt_store(vm* L, any* slot, any_t value) { rc::replace_adopt(L, *slot, value); }

	any_t LI_CC deopt_resume(vm* L, any* args, slot_t n_args, uint64_t resume_state) {
		constexpr uint64_t bytecode_mask  = (uint64_t{1} << 18) - 1;
		constexpr uint64_t handler_mask   = (uint64_t{1} << 19) - 1;
		auto               decode_handler = [](uint64_t encoded) { return encoded ? uint32_t(encoded - 1) : no_interpreter_handler; };

		uint32_t bytecode_pc = uint32_t(resume_state & bytecode_mask);
		uint32_t handler_pc  = decode_handler((resume_state >> 18) & handler_mask);
		uint32_t cleanup_pc  = decode_handler((resume_state >> 37) & handler_mask);
		any*     local0      = args + FRAME_SIZE + 1;
		if (!local0[FRAME_TARGET].is_fn() || !local0[FRAME_TARGET].as_fn()->is_virtual())
			return L->error("invalid deoptimization target");
		L->stack_top = local0 + local0[FRAME_TARGET].as_fn()->proto->num_locals;
		return vm_interpret_resume(L, args, n_args, bytecode_pc, handler_pc, cleanup_pc);
	}
#endif

	std::optional<uint32_t> pending_osr_target() noexcept {
#if LI_JIT
		if (osr_compile_target != bc::no_pos)
			return osr_compile_target;
#endif
		return std::nullopt;
	}

	std::optional<std::string> compile_required(vm* L, function* value) {
#if LI_JIT
		if (!L || !value || !value->is_virtual())
			return std::string("required JIT entry is not a virtual function");
		return lib::jit_on(L, value, L->jit_policy.verbose);
#else
		(void) L;
		(void) value;
		return std::string("native JIT is unavailable");
#endif
	}
}
