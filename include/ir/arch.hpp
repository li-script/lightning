#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <util/common.hpp>

#if LI_ARCH_X86 && !LI_32
	#include <ir/arch_x86.hpp>
#elif LI_ARCH_ARM && !LI_32
	#include <ir/arch_a64.hpp>
#else
// Non-JIT targets still parse the shared IR headers. They deliberately expose no
// allocatable physical registers.
namespace li::ir::arch {
	enum reg : int32_t { reg_none = 0 };
	inline constexpr std::array<reg, 0> gp_nonvolatile           = {};
	inline constexpr std::array<reg, 0> gp_volatile              = {};
	inline constexpr std::array<reg, 0> gp_argument              = {};
	inline constexpr std::array<reg, 0> fp_nonvolatile           = {};
	inline constexpr std::array<reg, 0> fp_volatile              = {};
	inline constexpr std::array<reg, 0> fp_argument              = {};
	inline constexpr std::array<reg, 0> gp_scratch               = {};
	inline constexpr std::array<reg, 0> fp_scratch               = {};
	inline constexpr reg                gp_retval                = reg_none;
	inline constexpr reg                fp_retval                = reg_none;
	inline constexpr reg                sp                       = reg_none;
	inline constexpr size_t             num_gp_reg               = 0;
	inline constexpr size_t             num_fp_reg               = 0;
	inline constexpr uint64_t           gp_allocatable_mask      = 0;
	inline constexpr uint64_t           fp_allocatable_mask      = 0;
	inline constexpr uint64_t           gp_volatile_mask         = 0;
	inline constexpr uint64_t           gp_nonvolatile_mask      = 0;
	inline constexpr uint64_t           fp_volatile_mask         = 0;
	inline constexpr uint64_t           fp_nonvolatile_mask      = 0;
	inline constexpr int32_t            stack_arg_begin          = 0;
	inline constexpr int32_t            home_size                = 0;
	inline constexpr bool               combined_arg_counter     = false;
	inline constexpr bool               independent_arg_counters = true;
	inline constexpr bool               packed_stack_arguments   = false;
	inline constexpr size_t             stack_alignment          = alignof(void*);
	inline constexpr reg                invalid                  = reg_none;

	constexpr bool        is_gp(reg) { return false; }
	constexpr bool        is_fp(reg) { return false; }
	constexpr bool        is_volatile(reg) { return false; }
	constexpr uint32_t    machine_id(reg) { return 0; }
	constexpr uint32_t    reg_number(reg) { return 0; }
	constexpr uint64_t    mask_of(reg) { return 0; }
	constexpr uint64_t    reg_mask(reg) { return 0; }
	constexpr reg         map_gp_arg(size_t, size_t) { return reg_none; }
	constexpr reg         map_fp_arg(size_t, size_t) { return reg_none; }
	constexpr const char* name_reg(reg) { return "none"; }
}
#endif
