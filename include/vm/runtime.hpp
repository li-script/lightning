#pragma once
#include <util/common.hpp>
#include <lang/types.hpp>

#if LI_ARCH_X86 && LI_32
	#define LI_C_CC __fastcall
#else
	#define LI_C_CC
#endif

// C builtins used for the VM runtime.
//
namespace li::runtime {
	array* LI_C_CC array_new(vm* L, uint32_t n);
	table* LI_C_CC table_new(vm* L, uint32_t n);
	uint64_t LI_C_CC field_set_raw(vm* L, uint64_t _unk, uint64_t _key, uint64_t _value);
	uint64_t LI_C_CC field_get_raw(vm* L, uint64_t _unk, uint64_t _key);
};