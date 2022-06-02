#pragma once
#include <algorithm>
#include <array>
#include <numeric>
#include <span>
#include <util/common.hpp>
#include <utility>

#if !LI_32 && LI_ARCH_X86
	#include <jit/zydis.hpp>
#endif

// Architectural constants.
//
namespace li::ir::arch {
	template<typename T>
	static constexpr std::span<const T> empty_set_v = {};

#if LI_ABI_MS64
	using native_reg                             = zy::reg;
	static constexpr native_reg gp_nonvolatile[] = {zy::RBP, zy::RSI, zy::RDI, zy::RBX, zy::R12, zy::R13, zy::R14, zy::R15};
	static constexpr native_reg gp_volatile[]    = {zy::RAX, zy::RCX, zy::RDX, zy::R8, zy::R9, zy::R10, zy::R11};
	static constexpr native_reg gp_argument[]    = {zy::RCX, zy::RDX, zy::R8, zy::R9};
	static constexpr native_reg gp_retval        = zy::RAX;
	static constexpr native_reg fp_nonvolatile[] = {zy::XMM6, zy::XMM7, zy::XMM8, zy::XMM9, zy::XMM10, zy::XMM11, zy::XMM12, zy::XMM13, zy::XMM14, zy::XMM15};
	static constexpr native_reg fp_volatile[]    = {zy::XMM0, zy::XMM1, zy::XMM2, zy::XMM3, zy::XMM4, zy::XMM5};
	static constexpr native_reg fp_argument[]    = {zy::XMM0, zy::XMM1, zy::XMM2, zy::XMM3};
	static constexpr native_reg fp_retval        = zy::XMM0;
	static constexpr native_reg sp               = zy::RSP;
	static constexpr native_reg bp               = zy::RBP;
	static constexpr native_reg invalid          = zy::NO_REG;

	static constexpr int32_t shadow_stack         = 0x20;
	static constexpr bool    combined_arg_counter = true;
#elif LI_ABI_SYSV64
	using native_reg                             = zy::reg;
	static constexpr native_reg gp_nonvolatile[] = {zy::RBP, zy::RBX, zy::R12, zy::R13, zy::R14, zy::R15};
	static constexpr native_reg gp_volatile[]    = {zy::RAX, zy::RDI, zy::RSI, zy::RDX, zy::RCX, zy::R8, zy::R9, zy::R10, zy::R11};
	static constexpr native_reg gp_argument[]    = {zy::RDI, zy::RSI, zy::RDX, zy::RCX, zy::R8, zy::R9};
	static constexpr native_reg gp_retval        = zy::RAX;
	static constexpr auto       fp_nonvolatile   = empty_set_v<native_reg>;
	static constexpr native_reg fp_volatile[]    = {zy::XMM0, zy::XMM1, zy::XMM2, zy::XMM3, zy::XMM4, zy::XMM5, zy::XMM6, zy::XMM7, zy::XMM8, zy::XMM9, zy::XMM10, zy::XMM11, zy::XMM12, zy::XMM13, zy::XMM14, zy::XMM15};
	static constexpr native_reg fp_argument[]    = {zy::XMM0, zy::XMM1, zy::XMM2, zy::XMM3, zy::XMM4, zy::XMM5, zy::XMM6, zy::XMM7};
	static constexpr native_reg fp_retval        = zy::XMM0;
	static constexpr native_reg sp               = zy::RSP;
	static constexpr native_reg bp               = zy::RBP;
	static constexpr native_reg invalid          = zy::NO_REG;

	static constexpr int32_t shadow_stack         = 0x20;
	static constexpr bool    combined_arg_counter = false;
#else
	using native_reg                           = int32_t;
	static constexpr auto       gp_nonvolatile = empty_set_v<native_reg>;
	static constexpr auto       gp_volatile    = empty_set_v<native_reg>;
	static constexpr auto       gp_argument    = empty_set_v<native_reg>;
	static constexpr native_reg gp_retval      = 0;
	static constexpr auto       fp_nonvolatile = empty_set_v<native_reg>;
	static constexpr auto       fp_volatile    = empty_set_v<native_reg>;
	static constexpr auto       fp_argument    = empty_set_v<native_reg>;
	static constexpr native_reg fp_retval      = 0;
	static constexpr native_reg sp             = 0;
	static constexpr native_reg bp             = 0;
	static constexpr native_reg invalid        = 0;

	static constexpr int32_t shadow_stack         = 0;
	static constexpr bool    combined_arg_counter = false;
#endif

	static constexpr size_t num_gp_reg = std::size(gp_volatile) + std::size(gp_nonvolatile);
	static constexpr size_t num_fp_reg = std::size(fp_volatile) + std::size(fp_nonvolatile);

	// Internal type.
	// - fpnonvol, fpvol < 0 none < +gpvol, gpnonvol.
	//
	using reg = int32_t;
	static constexpr bool is_volatile(reg r) {
		reg lim = (reg) std::size(gp_nonvolatile);
		if (r < 0) {
			lim = (reg) std::size(fp_nonvolatile);
			r   = -r;
		}
		return r <= lim;
	}

	// Translation between each.
	//
	static constexpr std::array<native_reg, num_fp_reg + 1 + num_gp_reg> virtual_to_native_map = []() {
		std::array<native_reg, num_fp_reg + 1 + num_gp_reg> res = {};
		size_t                                              it  = 0;
		for (native_reg r : fp_nonvolatile)
			res[it++] = r;
		for (native_reg r : fp_volatile)
			res[it++] = r;
		res[it++] = invalid;
		for (native_reg r : gp_volatile)
			res[it++] = r;
		for (native_reg r : gp_nonvolatile)
			res[it++] = r;
		return res;
	}();
	static constexpr native_reg to_native(reg i) {
		i += num_fp_reg;
		if (0 <= i && i < std::size(virtual_to_native_map)) {
			return virtual_to_native_map[i];
		}
		return invalid;
	}
	static constexpr reg from_native(native_reg n) {
		for (reg r = 0; r != virtual_to_native_map.size(); r++) {
			if (virtual_to_native_map[r] == n)
				return r - num_fp_reg;
		}
		return 0;
	}

	// Argument resolver.
	//
	static constexpr native_reg map_argument_native(size_t gp_arg_index, size_t fp_arg_index, bool fp) {
		if (!fp) {
			size_t idx = combined_arg_counter ? (gp_arg_index + fp_arg_index) : gp_arg_index;
			return std::size(gp_argument) < idx ? gp_argument[idx] : invalid;
		} else {
			size_t idx = combined_arg_counter ? (gp_arg_index + fp_arg_index) : fp_arg_index;
			return std::size(fp_argument) < idx ? fp_argument[idx] : invalid;
		}
	}
	static constexpr reg map_argument(size_t gp_arg_index, size_t fp_arg_index, bool fp) { return to_native(map_argument_native(gp_arg_index, fp_arg_index, fp)); };
};