#pragma once
#include <util/common.hpp>
#include <vm/types.hpp>

// Builtins used for the VM runtime.
//
// Every helper follows the native-call result contract: a successful result is
// owned by the caller, while an exception leaves its owned payload in
// vm::last_ex. Container getters remain borrowed. Raw helpers bypass traits
// and freeze for literal/construction bytecodes.
//
namespace li::runtime {
	any_t LI_CC field_set(vm* L, any_t target, any_t key, any_t value);
	any_t LI_CC field_get(vm* L, any_t target, any_t key);
	any_t LI_CC field_delete(vm* L, any_t target, any_t key);
	any_t LI_CC field_set_raw(vm* L, any_t target, any_t key, any_t value);
	any_t LI_CC field_get_raw(vm* L, any_t target, any_t key);
	any_t LI_CC capture_get(vm* L, function* target, int32_t index);
	any_t LI_CC capture_set(vm* L, function* target, int32_t index, any_t value);
};