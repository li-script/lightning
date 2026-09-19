#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace li::ir::arch {
	// Neutral physical IDs preserve the architectural encoding number: GP register
	// N is +(N+1), FP register N is -(N+1), and zero denotes no register.
	//
	enum reg : int32_t {
		reg_none = 0,

		rax = 1,
		rcx = 2,
		rdx = 3,
		rbx = 4,
		rsp = 5,
		rbp = 6,
		rsi = 7,
		rdi = 8,
		r8  = 9,
		r9  = 10,
		r10 = 11,
		r11 = 12,
		r12 = 13,
		r13 = 14,
		r14 = 15,
		r15 = 16,

		xmm0  = -1,
		xmm1  = -2,
		xmm2  = -3,
		xmm3  = -4,
		xmm4  = -5,
		xmm5  = -6,
		xmm6  = -7,
		xmm7  = -8,
		xmm8  = -9,
		xmm9  = -10,
		xmm10 = -11,
		xmm11 = -12,
		xmm12 = -13,
		xmm13 = -14,
		xmm14 = -15,
		xmm15 = -16,
	};

	constexpr bool       is_gp(reg r) { return r > 0; }
	constexpr bool       is_fp(reg r) { return r < 0; }
	constexpr uint32_t   machine_id(reg r) { return uint32_t(r < 0 ? -int32_t(r) - 1 : int32_t(r) - 1); }
	constexpr uint32_t   reg_number(reg r) { return machine_id(r); }
	constexpr uint64_t   mask_of(reg r) { return r == reg_none ? 0 : uint64_t{1} << machine_id(r); }
	constexpr uint64_t   reg_mask(reg r) { return mask_of(r); }
	inline constexpr reg invalid = reg_none;

	inline constexpr std::array gp_scratch = {r11};
	inline constexpr std::array fp_scratch = {xmm15};

#if LI_ABI_MS64
	// R11 and XMM15 are reserved for late x86 selection/legalization and therefore
	// intentionally absent from every allocatable set.
	inline constexpr std::array gp_nonvolatile       = {rbp, rbx, rsi, rdi, r12, r13, r14, r15};
	inline constexpr std::array gp_volatile          = {rax, rcx, rdx, r8, r9, r10};
	inline constexpr std::array gp_argument          = {rcx, rdx, r8, r9};
	inline constexpr reg        gp_retval            = rax;
	inline constexpr std::array fp_nonvolatile       = {xmm6, xmm7, xmm8, xmm9, xmm10, xmm11, xmm12, xmm13, xmm14};
	inline constexpr std::array fp_volatile          = {xmm0, xmm1, xmm2, xmm3, xmm4, xmm5};
	inline constexpr std::array fp_argument          = {xmm0, xmm1, xmm2, xmm3};
	inline constexpr reg        fp_retval            = xmm0;
	inline constexpr reg        sp                   = rsp;
	inline constexpr int32_t    stack_arg_begin      = 0x20;
	inline constexpr int32_t    home_size            = 0x20;
	inline constexpr bool       combined_arg_counter = true;
#else
	inline constexpr std::array         gp_nonvolatile = {rbp, rbx, r12, r13, r14, r15};
	inline constexpr std::array         gp_volatile    = {rax, rdi, rsi, rdx, rcx, r8, r9, r10};
	inline constexpr std::array         gp_argument    = {rdi, rsi, rdx, rcx, r8, r9};
	inline constexpr reg                gp_retval      = rax;
	inline constexpr std::array<reg, 0> fp_nonvolatile = {};
	inline constexpr std::array         fp_volatile    = {
		 xmm0,
		 xmm1,
		 xmm2,
		 xmm3,
		 xmm4,
		 xmm5,
		 xmm6,
		 xmm7,
		 xmm8,
		 xmm9,
		 xmm10,
		 xmm11,
		 xmm12,
		 xmm13,
		 xmm14,
	};
	inline constexpr std::array fp_argument          = {xmm0, xmm1, xmm2, xmm3, xmm4, xmm5, xmm6, xmm7};
	inline constexpr reg        fp_retval            = xmm0;
	inline constexpr reg        sp                   = rsp;
	inline constexpr int32_t    stack_arg_begin      = 0;
	inline constexpr int32_t    home_size            = 0;
	inline constexpr bool       combined_arg_counter = false;
#endif
	inline constexpr bool   packed_stack_arguments   = false;
	inline constexpr bool   independent_arg_counters = !combined_arg_counter;
	inline constexpr size_t stack_alignment          = 16;

	inline constexpr size_t num_gp_reg = gp_volatile.size() + gp_nonvolatile.size();
	inline constexpr size_t num_fp_reg = fp_volatile.size() + fp_nonvolatile.size();

	template<size_t N>
	constexpr uint64_t register_mask(const std::array<reg, N>& registers) {
		uint64_t result = 0;
		for (reg r : registers)
			result |= mask_of(r);
		return result;
	}
	inline constexpr uint64_t gp_volatile_mask    = register_mask(gp_volatile);
	inline constexpr uint64_t gp_nonvolatile_mask = register_mask(gp_nonvolatile);
	inline constexpr uint64_t fp_volatile_mask    = register_mask(fp_volatile);
	inline constexpr uint64_t fp_nonvolatile_mask = register_mask(fp_nonvolatile);
	inline constexpr uint64_t gp_allocatable_mask = gp_volatile_mask | gp_nonvolatile_mask;
	inline constexpr uint64_t fp_allocatable_mask = fp_volatile_mask | fp_nonvolatile_mask;
	inline constexpr uint64_t gp_scratch_mask     = register_mask(gp_scratch);
	inline constexpr uint64_t fp_scratch_mask     = register_mask(fp_scratch);

	constexpr bool is_volatile(reg r) { return (mask_of(r) & (is_fp(r) ? fp_volatile_mask : gp_volatile_mask)) != 0; }
	constexpr reg  map_gp_arg(size_t gp_arg_index, size_t fp_arg_index) {
		size_t idx = combined_arg_counter ? gp_arg_index + fp_arg_index : gp_arg_index;
		return idx < gp_argument.size() ? gp_argument[idx] : reg_none;
	}
	constexpr reg map_fp_arg(size_t gp_arg_index, size_t fp_arg_index) {
		size_t idx = combined_arg_counter ? gp_arg_index + fp_arg_index : fp_arg_index;
		return idx < fp_argument.size() ? fp_argument[idx] : reg_none;
	}

	constexpr const char* name_reg(reg r) {
		switch (r) {
			case rax:
				return "rax";
			case rcx:
				return "rcx";
			case rdx:
				return "rdx";
			case rbx:
				return "rbx";
			case rsp:
				return "rsp";
			case rbp:
				return "rbp";
			case rsi:
				return "rsi";
			case rdi:
				return "rdi";
			case r8:
				return "r8";
			case r9:
				return "r9";
			case r10:
				return "r10";
			case r11:
				return "r11";
			case r12:
				return "r12";
			case r13:
				return "r13";
			case r14:
				return "r14";
			case r15:
				return "r15";
			case xmm0:
				return "xmm0";
			case xmm1:
				return "xmm1";
			case xmm2:
				return "xmm2";
			case xmm3:
				return "xmm3";
			case xmm4:
				return "xmm4";
			case xmm5:
				return "xmm5";
			case xmm6:
				return "xmm6";
			case xmm7:
				return "xmm7";
			case xmm8:
				return "xmm8";
			case xmm9:
				return "xmm9";
			case xmm10:
				return "xmm10";
			case xmm11:
				return "xmm11";
			case xmm12:
				return "xmm12";
			case xmm13:
				return "xmm13";
			case xmm14:
				return "xmm14";
			case xmm15:
				return "xmm15";
			default:
				return "none";
		}
	}
}
