#include <atomic>
#include <cmath>
#include <ir/runtime.hpp>
#include <lang/operator.hpp>
#include <vm/iterator.hpp>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/runtime.hpp>

namespace li::ir::runtime {
	any_t LI_CC unary(vm* L, any_t value, int32_t op) { return apply_unary(L, value, static_cast<bc::opcode>(op)); }

	any_t LI_CC binary(vm* L, any_t lhs, any_t rhs, int32_t op) { return apply_binary(L, lhs, rhs, static_cast<bc::opcode>(op)); }

	any_t LI_CC field_get(vm* L, any_t container, any_t key) { return li::runtime::field_get(L, container, key); }

	any_t LI_CC field_set(vm* L, any_t container, any_t key, any_t value) { return li::runtime::field_set(L, container, key, value); }

	any_t LI_CC field_get_raw(vm* L, any_t container, any_t key) { return li::runtime::field_get_raw(L, container, key); }

	any_t LI_CC field_get_raw_borrowed(vm* L, any_t container, any_t key) {
		any result = li::runtime::field_get_raw(L, container, key);
		if (!result.is_exc())
			rc::release(L, result);
		return result;
	}

	any_t LI_CC field_set_raw(vm* L, any_t container, any_t key, any_t value) { return li::runtime::field_set_raw(L, container, key, value); }

	const nfunc_info unary_info = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 "ir.unary",
		 {nfunc_overload{li::bit_cast<const void*>(&unary), {type::any, type::i32}, type::any}},
	};
	const nfunc_info binary_info = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 "ir.binary",
		 {nfunc_overload{li::bit_cast<const void*>(&binary), {type::any, type::any, type::i32}, type::any}},
	};
	const nfunc_info field_get_info = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 "ir.field_get",
		 {nfunc_overload{li::bit_cast<const void*>(&field_get), {type::any, type::any}, type::any}},
	};
	const nfunc_info field_set_info = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 "ir.field_set",
		 {nfunc_overload{li::bit_cast<const void*>(&field_set), {type::any, type::any, type::any}, type::any}},
	};
	const nfunc_info field_get_raw_info = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 "ir.field_get_raw",
		 {nfunc_overload{li::bit_cast<const void*>(&field_get_raw), {type::any, type::any}, type::any}},
	};
	const nfunc_info field_set_raw_info = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 "ir.field_set_raw",
		 {nfunc_overload{li::bit_cast<const void*>(&field_set_raw), {type::any, type::any, type::any}, type::any}},
	};
	const nfunc_info capture_get_info = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 "ir.capture_get",
		 {nfunc_overload{li::bit_cast<const void*>(&li::runtime::capture_get), {type::fn, type::i32}, type::any}},
	};
	const nfunc_info capture_set_info = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 "ir.capture_set",
		 {nfunc_overload{li::bit_cast<const void*>(&li::runtime::capture_set), {type::fn, type::i32, type::any}, type::any}},
	};
	const nfunc_info forced_unwind_info = {
		 func_attr_c_takes_vm,
		 "ir.forced_unwind_requested",
		 {nfunc_overload{li::bit_cast<const void*>(&forced_unwind_requested), {}, type::i1}},
	};

	any_t LI_CC va_get(vm* L, any* args, slot_t n_args, any_t index) {
		if (!index.is_num())
			return L->error("variable argument index must be a non-negative integer");

		const number value = index.as_num();
		if (!std::isfinite(value) || value < 0 || value != std::trunc(value))
			return L->error("variable argument index must be a non-negative integer");
		if (value >= number(n_args))
			return L->ok();

		const slot_t offset = static_cast<slot_t>(value);
		any          result = args[-offset];
		rc::retain_frame(L, result);
		return result;
	}

	any_t LI_CC class_new(vm* L, any_t value) {
		if (!value.is_vcl())
			return L->error("instantiating non-class");
		return any(object::create(L, value.as_vcl()));
	}

	uint8_t LI_CC forced_unwind_requested(vm* L) { return L->forced_unwind_requested(); }

	uint8_t LI_CC class_is(any_t value, any_t base) { return base.is_vcl() && class_matches(value, base.as_vcl()->identity); }

	const nfunc_info class_matches_info = {
		 func_attr_pure,
		 "ir.class_matches",
		 {nfunc_overload{li::bit_cast<const void*>(&class_matches), {type::any, type::i64}, type::i1}},
	};

	any_t LI_CC iter_next(vm* L, any_t target, any* local0, int32_t state_slot) {
		any& state = local0[state_slot];
		any& key   = local0[state_slot + 1];
		any& value = local0[state_slot + 2];
		return li::iterator_step(L, target, state, key, value);
	}

	static void check_stack_range(vm* L, any* begin, slot_t slots) {
		const uintptr_t stack_begin = reinterpret_cast<uintptr_t>(L->stack);
		const uintptr_t stack_end   = reinterpret_cast<uintptr_t>(L->stack_limit);
		const uintptr_t range_begin = reinterpret_cast<uintptr_t>(begin);
		const uintptr_t range_end   = range_begin + uintptr_t(slots) * sizeof(any);
		if (range_begin < stack_begin || range_end < range_begin || range_end > stack_end)
			L->panic("stack too large");
	}

	void LI_CC frame_enter(vm* L, any* local0, slot_t num_locals, slot_t scratch_slots) {
		if (L->stack_top != local0)
			L->panic("invalid JIT entry frame");
		check_stack_range(L, local0, num_locals + scratch_slots);
		fill_nil(local0, num_locals + scratch_slots);
		L->stack_top = local0 + num_locals;
	}

	uint64_t LI_CC make_call_frame(vm* L, any* local0, uint32_t caller_pc) {
		check_stack_range(L, local0, 0);
		if (caller_pc > BC_MAX_IP)
			L->panic("invalid JIT caller pc");
		call_frame frame{
			 .caller_pc = caller_pc,
			 .stack_pos = msize_t(local0 - L->stack),
		};
		return li::bit_cast<uint64_t>(frame);
	}

	void LI_CC call_prepare(vm* L, any* scratch, slot_t owned_slots, any* child_local0) {
		if (L->stack_top != scratch || child_local0 < scratch || child_local0 - scratch != owned_slots + 1)
			L->panic("invalid JIT call frame");
		check_stack_range(L, scratch, owned_slots + 1);
		// Call lowering has already retained borrowed dynamic operands. Owned
		// operands are adopted directly by the frame; immediates need no action.
		L->stack_top = child_local0;
	}

	any_t LI_CC call_finish(vm* L, any* local0, any_t result) {
		L->truncate_stack(local0);
		return result;
	}

	any_t LI_CC frame_leave(vm* L, any* local0, any_t result) {
		if (!rc::check_store(L, result)) {
			L->truncate_stack(local0);
			return exception_marker;
		}
		L->truncate_stack(local0);
		return result;
	}

	void LI_CC slot_replace(vm* L, any* slot, any_t value) { rc::replace_frame(L, *slot, value); }

	any_t LI_CC slot_try_replace(vm* L, any* slot, any_t value) {
		if (!rc::try_replace(L, *slot, value))
			return exception_marker;
		return L->ok();
	}

	any_t LI_CC set_exception(vm* L, any_t value, any* local0, uint32_t source_bc) {
		check_stack_range(L, local0, 0);
		any target = local0[FRAME_TARGET];
		if (!target.is_fn() || !target.as_fn()->is_virtual())
			return L->error("invalid JIT exception frame");
		return L->set_exception(value, target.as_fn(), source_bc, local0);
	}

	void LI_CC safepoint(vm* L, any* stack_top) {
		check_stack_range(L, stack_top, 0);
		L->stack_top = stack_top;
		// Publish a coherent VM-stack boundary to asynchronous profiling hooks.
		std::atomic_signal_fence(std::memory_order_seq_cst);
	}
}
