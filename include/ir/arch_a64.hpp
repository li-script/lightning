#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <util/common.hpp>

namespace li::ir::a64::abi {
	using register_id = uint8_t;

	enum class variadic_style : uint8_t {
		aapcs64_register_save_areas,
		apple_stack_arguments,
	};

	inline constexpr std::array<register_id, 8> gp_argument = {0, 1, 2, 3, 4, 5, 6, 7};
	inline constexpr std::array<register_id, 8> fp_argument = {0, 1, 2, 3, 4, 5, 6, 7};
	inline constexpr std::array<register_id, 2> gp_result   = {0, 1};
	inline constexpr std::array<register_id, 4> fp_result   = {0, 1, 2, 3};

	inline constexpr std::array<register_id, 10> gp_callee_saved = {19, 20, 21, 22, 23, 24, 25, 26, 27, 28};
	inline constexpr std::array<register_id, 8>  fp_callee_saved = {8, 9, 10, 11, 12, 13, 14, 15};
	inline constexpr std::array<register_id, 19> gp_caller_saved = {
		 0,
		 1,
		 2,
		 3,
		 4,
		 5,
		 6,
		 7,
		 8,
		 9,
		 10,
		 11,
		 12,
		 13,
		 14,
		 15,
		 16,
		 17,
		 18,
	};
	inline constexpr std::array<register_id, 24> fp_caller_saved = {
		 0,
		 1,
		 2,
		 3,
		 4,
		 5,
		 6,
		 7,
		 16,
		 17,
		 18,
		 19,
		 20,
		 21,
		 22,
		 23,
		 24,
		 25,
		 26,
		 27,
		 28,
		 29,
		 30,
		 31,
	};

	inline constexpr std::array<register_id, 2> intra_procedure_call = {16, 17};
	inline constexpr std::array<register_id, 1> veneer_scratch       = {16};
	inline constexpr std::array<register_id, 1> fp_scratch           = {31};

	inline constexpr register_id indirect_result = 8;
	inline constexpr register_id platform        = 18;
	inline constexpr register_id frame_pointer   = 29;
	inline constexpr register_id link            = 30;
	inline constexpr register_id stack_pointer   = 31;

#if defined(__APPLE__)
	inline constexpr bool           x18_reserved           = true;
	inline constexpr bool           packed_stack_arguments = true;
	inline constexpr variadic_style variadics              = variadic_style::apple_stack_arguments;
#else
	inline constexpr bool           x18_reserved           = false;
	inline constexpr bool           packed_stack_arguments = false;
	inline constexpr variadic_style variadics              = variadic_style::aapcs64_register_save_areas;
#endif

	inline constexpr size_t stack_alignment              = 16;
	inline constexpr size_t home_size                    = 0;
	inline constexpr size_t generic_stack_argument_slot  = 8;
	inline constexpr size_t gp_variadic_save_area        = 8 * sizeof(uint64_t);
	inline constexpr size_t fp_variadic_save_area        = 8 * 16;
	inline constexpr size_t fp_callee_saved_bytes        = 8;
	inline constexpr bool   separate_gp_fp_arg_counters  = true;
	inline constexpr bool   anonymous_variadics_on_stack = variadics == variadic_style::apple_stack_arguments;

	inline constexpr uint32_t mask(std::span<const register_id> registers) {
		uint32_t result = 0;
		for (register_id value : registers)
			result |= uint32_t{1} << value;
		return result;
	}

	inline constexpr uint32_t gp_callee_saved_mask = mask(gp_callee_saved);
	inline constexpr uint32_t fp_callee_saved_mask = mask(fp_callee_saved);
	inline constexpr uint32_t gp_caller_saved_mask = mask(gp_caller_saved);
	inline constexpr uint32_t fp_caller_saved_mask = mask(fp_caller_saved);
	inline constexpr uint32_t aapcs64_reserved_gp_mask =
		 (uint32_t{1} << 16) | (uint32_t{1} << 17) | (uint32_t{1} << 29) | (uint32_t{1} << 30) | (uint32_t{1} << 31);
	inline constexpr uint32_t apple_reserved_gp_mask = aapcs64_reserved_gp_mask | (uint32_t{1} << 18);
	inline constexpr uint32_t reserved_gp_mask       = x18_reserved ? apple_reserved_gp_mask : aapcs64_reserved_gp_mask;
	inline constexpr uint32_t allocatable_gp_mask    = ~reserved_gp_mask;
	inline constexpr uint32_t allocatable_fp_mask    = ~(uint32_t{1} << 31);

	struct description {
		std::span<const register_id> gp_args;
		std::span<const register_id> fp_args;
		std::span<const register_id> gp_returns;
		std::span<const register_id> fp_returns;
		std::span<const register_id> gp_nonvolatile;
		std::span<const register_id> fp_nonvolatile;
		uint32_t                     gp_allocatable;
		uint32_t                     fp_allocatable;
		register_id                  gp_return;
		register_id                  fp_return;
		register_id                  sp;
		register_id                  fp;
		register_id                  lr;
		size_t                       stack_align;
		size_t                       stack_arg_begin;
		size_t                       home_bytes;
		bool                         independent_arg_counters;
		bool                         packed_stack_args;
		bool                         reserve_x18;
		variadic_style               variadic_arguments;
	};

	inline constexpr description aapcs64 = {
		 gp_argument,
		 fp_argument,
		 gp_result,
		 fp_result,
		 gp_callee_saved,
		 fp_callee_saved,
		 ~aapcs64_reserved_gp_mask,
		 allocatable_fp_mask,
		 0,
		 0,
		 stack_pointer,
		 frame_pointer,
		 link,
		 stack_alignment,
		 0,
		 home_size,
		 separate_gp_fp_arg_counters,
		 false,
		 false,
		 variadic_style::aapcs64_register_save_areas,
	};
	inline constexpr description apple_aapcs64 = {
		 gp_argument,
		 fp_argument,
		 gp_result,
		 fp_result,
		 gp_callee_saved,
		 fp_callee_saved,
		 ~apple_reserved_gp_mask,
		 allocatable_fp_mask,
		 0,
		 0,
		 stack_pointer,
		 frame_pointer,
		 link,
		 stack_alignment,
		 0,
		 home_size,
		 separate_gp_fp_arg_counters,
		 true,
		 true,
		 variadic_style::apple_stack_arguments,
	};

#if defined(__APPLE__)
	inline constexpr description target = apple_aapcs64;
#else
	inline constexpr description target = aapcs64;
#endif
}

#if LI_ARCH_ARM && !LI_32
namespace li::ir::arch {
	// Neutral physical IDs are stable architectural IDs: Xn is n+1 and Vn is -(n+1).
	// X16/X17 and V31 are backend scratch registers. Apple additionally reserves X18.
	enum reg : int32_t { reg_none = 0 };

	inline constexpr reg      gp(uint8_t number) { return reg(int32_t(number) + 1); }
	inline constexpr reg      fp(uint8_t number) { return reg(-int32_t(number) - 1); }
	inline constexpr bool     is_gp(reg value) { return value > reg_none; }
	inline constexpr bool     is_fp(reg value) { return value < reg_none; }
	inline constexpr uint8_t  reg_number(reg value) { return uint8_t(value > 0 ? int32_t(value) - 1 : -int32_t(value) - 1); }
	inline constexpr uint32_t machine_id(reg value) { return reg_number(value); }
	inline constexpr uint64_t reg_mask(reg value) { return value == reg_none ? 0 : uint64_t{1} << reg_number(value); }
	inline constexpr uint64_t mask_of(reg value) { return reg_mask(value); }

	inline constexpr std::array<reg, 2> gp_scratch = {gp(16), gp(17)};
	inline constexpr std::array<reg, 1> fp_scratch = {fp(31)};

	#if defined(__APPLE__)
	inline constexpr std::array<reg, 16> gp_volatile = {
		 gp(0),
		 gp(1),
		 gp(2),
		 gp(3),
		 gp(4),
		 gp(5),
		 gp(6),
		 gp(7),
		 gp(8),
		 gp(9),
		 gp(10),
		 gp(11),
		 gp(12),
		 gp(13),
		 gp(14),
		 gp(15),
	};
	#else
	inline constexpr std::array<reg, 17> gp_volatile = {
		 gp(0),
		 gp(1),
		 gp(2),
		 gp(3),
		 gp(4),
		 gp(5),
		 gp(6),
		 gp(7),
		 gp(8),
		 gp(9),
		 gp(10),
		 gp(11),
		 gp(12),
		 gp(13),
		 gp(14),
		 gp(15),
		 gp(18),
	};
	#endif
	inline constexpr std::array<reg, 10> gp_nonvolatile = {
		 gp(19),
		 gp(20),
		 gp(21),
		 gp(22),
		 gp(23),
		 gp(24),
		 gp(25),
		 gp(26),
		 gp(27),
		 gp(28),
	};
	inline constexpr std::array<reg, 23> fp_volatile = {
		 fp(0),
		 fp(1),
		 fp(2),
		 fp(3),
		 fp(4),
		 fp(5),
		 fp(6),
		 fp(7),
		 fp(16),
		 fp(17),
		 fp(18),
		 fp(19),
		 fp(20),
		 fp(21),
		 fp(22),
		 fp(23),
		 fp(24),
		 fp(25),
		 fp(26),
		 fp(27),
		 fp(28),
		 fp(29),
		 fp(30),
	};
	inline constexpr std::array<reg, 8> fp_nonvolatile = {
		 fp(8),
		 fp(9),
		 fp(10),
		 fp(11),
		 fp(12),
		 fp(13),
		 fp(14),
		 fp(15),
	};

	inline constexpr std::array<reg, 8> gp_argument = {
		 gp(0),
		 gp(1),
		 gp(2),
		 gp(3),
		 gp(4),
		 gp(5),
		 gp(6),
		 gp(7),
	};
	inline constexpr std::array<reg, 8> fp_argument = {
		 fp(0),
		 fp(1),
		 fp(2),
		 fp(3),
		 fp(4),
		 fp(5),
		 fp(6),
		 fp(7),
	};

	inline constexpr size_t num_gp_reg = gp_volatile.size() + gp_nonvolatile.size();
	inline constexpr size_t num_fp_reg = fp_volatile.size() + fp_nonvolatile.size();
	inline constexpr reg    gp_retval  = gp(0);
	inline constexpr reg    fp_retval  = fp(0);
	inline constexpr reg    sp         = gp(31);
	inline constexpr reg    invalid    = reg_none;

	inline constexpr int32_t stack_arg_begin          = 0;
	inline constexpr int32_t home_size                = 0;
	inline constexpr int32_t stack_alignment          = 16;
	inline constexpr bool    combined_arg_counter     = false;
	inline constexpr bool    independent_arg_counters = true;
	inline constexpr bool    packed_stack_arguments   = a64::abi::packed_stack_arguments;

	inline constexpr uint64_t gp_volatile_mask = [] {
		uint64_t result = 0;
		for (reg value : gp_volatile)
			result |= reg_mask(value);
		return result;
	}();
	inline constexpr uint64_t gp_nonvolatile_mask = [] {
		uint64_t result = 0;
		for (reg value : gp_nonvolatile)
			result |= reg_mask(value);
		return result;
	}();
	inline constexpr uint64_t fp_volatile_mask = [] {
		uint64_t result = 0;
		for (reg value : fp_volatile)
			result |= reg_mask(value);
		return result;
	}();
	inline constexpr uint64_t fp_nonvolatile_mask = [] {
		uint64_t result = 0;
		for (reg value : fp_nonvolatile)
			result |= reg_mask(value);
		return result;
	}();
	inline constexpr uint64_t gp_allocatable_mask = gp_volatile_mask | gp_nonvolatile_mask;
	inline constexpr uint64_t fp_allocatable_mask = fp_volatile_mask | fp_nonvolatile_mask;
	inline constexpr uint64_t gp_scratch_mask     = reg_mask(gp_scratch[0]) | reg_mask(gp_scratch[1]);
	inline constexpr uint64_t fp_scratch_mask     = reg_mask(fp_scratch[0]);

	inline constexpr bool is_volatile(reg value) {
		uint64_t mask = is_gp(value) ? gp_volatile_mask : fp_volatile_mask;
		return value != reg_none && (mask & reg_mask(value)) != 0;
	}
	inline constexpr reg map_gp_arg(size_t gp_index, size_t) { return gp_index < gp_argument.size() ? gp_argument[gp_index] : reg_none; }
	inline constexpr reg map_fp_arg(size_t, size_t fp_index) { return fp_index < fp_argument.size() ? fp_argument[fp_index] : reg_none; }

	inline constexpr std::array<const char*, 32> gp_names = {
		 "x0",
		 "x1",
		 "x2",
		 "x3",
		 "x4",
		 "x5",
		 "x6",
		 "x7",
		 "x8",
		 "x9",
		 "x10",
		 "x11",
		 "x12",
		 "x13",
		 "x14",
		 "x15",
		 "x16",
		 "x17",
		 "x18",
		 "x19",
		 "x20",
		 "x21",
		 "x22",
		 "x23",
		 "x24",
		 "x25",
		 "x26",
		 "x27",
		 "x28",
		 "x29",
		 "x30",
		 "sp",
	};
	inline constexpr std::array<const char*, 32> fp_names = {
		 "v0",
		 "v1",
		 "v2",
		 "v3",
		 "v4",
		 "v5",
		 "v6",
		 "v7",
		 "v8",
		 "v9",
		 "v10",
		 "v11",
		 "v12",
		 "v13",
		 "v14",
		 "v15",
		 "v16",
		 "v17",
		 "v18",
		 "v19",
		 "v20",
		 "v21",
		 "v22",
		 "v23",
		 "v24",
		 "v25",
		 "v26",
		 "v27",
		 "v28",
		 "v29",
		 "v30",
		 "v31",
	};
	inline constexpr const char* name_reg(reg value) {
		if (value == reg_none)
			return "none";
		return is_gp(value) ? gp_names[reg_number(value)] : fp_names[reg_number(value)];
	}
}
#endif
