#pragma once
#include <util/common.hpp>
#include <lang/types.hpp>

// Builtins used for the VM runtime.
//
namespace li::runtime {
	any_t LI_CC field_set_raw(vm* L, any_t unk, any_t key, any_t value);
	any_t LI_CC field_get_raw(vm* L, any_t unk, any_t key);
};