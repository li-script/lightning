#pragma once
#include <vm/function.hpp>

namespace li::ir::runtime {
	// Generic slow paths. Inputs are borrowed and successful results are owned.
	any_t LI_CC unary(vm* L, any_t value, int32_t op);
	any_t LI_CC binary(vm* L, any_t lhs, any_t rhs, int32_t op);
	any_t LI_CC field_get(vm* L, any_t container, any_t key);
	any_t LI_CC field_set(vm* L, any_t container, any_t key, any_t value);
	any_t LI_CC field_get_raw(vm* L, any_t container, any_t key);
	any_t LI_CC field_get_raw_borrowed(vm* L, any_t container, any_t key);
	any_t LI_CC field_set_raw(vm* L, any_t container, any_t key, any_t value);

	extern const nfunc_info unary_info;
	extern const nfunc_info binary_info;
	extern const nfunc_info field_get_info;
	extern const nfunc_info field_set_info;
	extern const nfunc_info field_get_raw_info;
	extern const nfunc_info field_set_raw_info;
	extern const nfunc_info capture_get_info;
	extern const nfunc_info capture_set_info;
	extern const nfunc_info forced_unwind_info;
	extern const nfunc_info class_matches_info;

	// Helpers for operations whose physical ABI is more compact than their IR shape.
	any_t LI_CC   va_get(vm* L, any* args, slot_t n_args, any_t index);
	any_t LI_CC   class_new(vm* L, any_t value);
	uint8_t LI_CC class_is(any_t value, any_t base);
	// Mutates the owning state/key/value frame slots beginning at state_slot.
	any_t LI_CC   iter_next(vm* L, any_t target, any* local0, int32_t state_slot);
	uint8_t LI_CC forced_unwind_requested(vm* L);

	// Owning VM-frame transitions used by generated methods. call_prepare only
	// publishes slots: lowering retains borrowed dynamic operands and transfers
	// explicitly marked owned operands before publication.
	void LI_CC     frame_enter(vm* L, any* local0, slot_t num_locals, slot_t scratch_slots);
	uint64_t LI_CC make_call_frame(vm* L, any* local0, uint32_t caller_pc);
	void LI_CC     call_prepare(vm* L, any* scratch, slot_t owned_slots, any* child_local0);
	any_t LI_CC    call_finish(vm* L, any* local0, any_t result);
	any_t LI_CC    frame_leave(vm* L, any* local0, any_t result);
	void LI_CC     slot_replace(vm* L, any* slot, any_t value);
	any_t LI_CC    slot_try_replace(vm* L, any* slot, any_t value);
	any_t LI_CC    set_exception(vm* L, any_t value, any* local0, uint32_t source_bc);
	void LI_CC     safepoint(vm* L, any* stack_top);
}
