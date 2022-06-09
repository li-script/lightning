#pragma once
#include <util/common.hpp>
#include <lang/types.hpp>

// Builtins used for the VM runtime.
//
namespace li::runtime {
	array* LI_CC   array_new(vm* L, msize_t n);
	table* LI_CC   table_new(vm* L, msize_t n);
	uint64_t LI_CC field_set_raw(vm* L, uint64_t _unk, uint64_t _key, uint64_t _value);
	uint64_t LI_CC field_get_raw(vm* L, uint64_t _unk, uint64_t _key);
};