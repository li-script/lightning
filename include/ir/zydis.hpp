#pragma once
#include <util/common.hpp>
#if !LI_ARCH_X86 || LI_32
	#error "JIT mode is only available for x86-64."
#endif
#include <span>
#include <vector>
#include <string>
#include <optional>
#include <Zycore/LibC.h>
#include <Zydis/Zydis.h>

namespace li::zy {
	// Rename registers.
	//
	using reg = ZydisRegister;
	static constexpr auto NO_REG = ZYDIS_REGISTER_NONE;
	static constexpr auto AL     = ZYDIS_REGISTER_AL;
	static constexpr auto CL     = ZYDIS_REGISTER_CL;
	static constexpr auto DL     = ZYDIS_REGISTER_DL;
	static constexpr auto BL     = ZYDIS_REGISTER_BL;
	static constexpr auto AH     = ZYDIS_REGISTER_AH;
	static constexpr auto CH     = ZYDIS_REGISTER_CH;
	static constexpr auto DH     = ZYDIS_REGISTER_DH;
	static constexpr auto BH     = ZYDIS_REGISTER_BH;
	static constexpr auto SPL    = ZYDIS_REGISTER_SPL;
	static constexpr auto BPL    = ZYDIS_REGISTER_BPL;
	static constexpr auto SIL    = ZYDIS_REGISTER_SIL;
	static constexpr auto DIL    = ZYDIS_REGISTER_DIL;
	static constexpr auto R8B    = ZYDIS_REGISTER_R8B;
	static constexpr auto R9B    = ZYDIS_REGISTER_R9B;
	static constexpr auto R10B   = ZYDIS_REGISTER_R10B;
	static constexpr auto R11B   = ZYDIS_REGISTER_R11B;
	static constexpr auto R12B   = ZYDIS_REGISTER_R12B;
	static constexpr auto R13B   = ZYDIS_REGISTER_R13B;
	static constexpr auto R14B   = ZYDIS_REGISTER_R14B;
	static constexpr auto R15B   = ZYDIS_REGISTER_R15B;
	static constexpr auto AX     = ZYDIS_REGISTER_AX;
	static constexpr auto CX     = ZYDIS_REGISTER_CX;
	static constexpr auto DX     = ZYDIS_REGISTER_DX;
	static constexpr auto BX     = ZYDIS_REGISTER_BX;
	static constexpr auto SP     = ZYDIS_REGISTER_SP;
	static constexpr auto BP     = ZYDIS_REGISTER_BP;
	static constexpr auto SI     = ZYDIS_REGISTER_SI;
	static constexpr auto DI     = ZYDIS_REGISTER_DI;
	static constexpr auto R8W    = ZYDIS_REGISTER_R8W;
	static constexpr auto R9W    = ZYDIS_REGISTER_R9W;
	static constexpr auto R10W   = ZYDIS_REGISTER_R10W;
	static constexpr auto R11W   = ZYDIS_REGISTER_R11W;
	static constexpr auto R12W   = ZYDIS_REGISTER_R12W;
	static constexpr auto R13W   = ZYDIS_REGISTER_R13W;
	static constexpr auto R14W   = ZYDIS_REGISTER_R14W;
	static constexpr auto R15W   = ZYDIS_REGISTER_R15W;
	static constexpr auto EAX    = ZYDIS_REGISTER_EAX;
	static constexpr auto ECX    = ZYDIS_REGISTER_ECX;
	static constexpr auto EDX    = ZYDIS_REGISTER_EDX;
	static constexpr auto EBX    = ZYDIS_REGISTER_EBX;
	static constexpr auto ESP    = ZYDIS_REGISTER_ESP;
	static constexpr auto EBP    = ZYDIS_REGISTER_EBP;
	static constexpr auto ESI    = ZYDIS_REGISTER_ESI;
	static constexpr auto EDI    = ZYDIS_REGISTER_EDI;
	static constexpr auto R8D    = ZYDIS_REGISTER_R8D;
	static constexpr auto R9D    = ZYDIS_REGISTER_R9D;
	static constexpr auto R10D   = ZYDIS_REGISTER_R10D;
	static constexpr auto R11D   = ZYDIS_REGISTER_R11D;
	static constexpr auto R12D   = ZYDIS_REGISTER_R12D;
	static constexpr auto R13D   = ZYDIS_REGISTER_R13D;
	static constexpr auto R14D   = ZYDIS_REGISTER_R14D;
	static constexpr auto R15D   = ZYDIS_REGISTER_R15D;
	static constexpr auto RAX    = ZYDIS_REGISTER_RAX;
	static constexpr auto RCX    = ZYDIS_REGISTER_RCX;
	static constexpr auto RDX    = ZYDIS_REGISTER_RDX;
	static constexpr auto RBX    = ZYDIS_REGISTER_RBX;
	static constexpr auto RSP    = ZYDIS_REGISTER_RSP;
	static constexpr auto RBP    = ZYDIS_REGISTER_RBP;
	static constexpr auto RSI    = ZYDIS_REGISTER_RSI;
	static constexpr auto RDI    = ZYDIS_REGISTER_RDI;
	static constexpr auto R8     = ZYDIS_REGISTER_R8;
	static constexpr auto R9     = ZYDIS_REGISTER_R9;
	static constexpr auto R10    = ZYDIS_REGISTER_R10;
	static constexpr auto R11    = ZYDIS_REGISTER_R11;
	static constexpr auto R12    = ZYDIS_REGISTER_R12;
	static constexpr auto R13    = ZYDIS_REGISTER_R13;
	static constexpr auto R14    = ZYDIS_REGISTER_R14;
	static constexpr auto R15    = ZYDIS_REGISTER_R15;
	static constexpr auto XMM0   = ZYDIS_REGISTER_XMM0;
	static constexpr auto XMM1   = ZYDIS_REGISTER_XMM1;
	static constexpr auto XMM2   = ZYDIS_REGISTER_XMM2;
	static constexpr auto XMM3   = ZYDIS_REGISTER_XMM3;
	static constexpr auto XMM4   = ZYDIS_REGISTER_XMM4;
	static constexpr auto XMM5   = ZYDIS_REGISTER_XMM5;
	static constexpr auto XMM6   = ZYDIS_REGISTER_XMM6;
	static constexpr auto XMM7   = ZYDIS_REGISTER_XMM7;
	static constexpr auto XMM8   = ZYDIS_REGISTER_XMM8;
	static constexpr auto XMM9   = ZYDIS_REGISTER_XMM9;
	static constexpr auto XMM10  = ZYDIS_REGISTER_XMM10;
	static constexpr auto XMM11  = ZYDIS_REGISTER_XMM11;
	static constexpr auto XMM12  = ZYDIS_REGISTER_XMM12;
	static constexpr auto XMM13  = ZYDIS_REGISTER_XMM13;
	static constexpr auto XMM14  = ZYDIS_REGISTER_XMM14;
	static constexpr auto XMM15  = ZYDIS_REGISTER_XMM15;
	static constexpr auto YMM0   = ZYDIS_REGISTER_YMM0;
	static constexpr auto YMM1   = ZYDIS_REGISTER_YMM1;
	static constexpr auto YMM2   = ZYDIS_REGISTER_YMM2;
	static constexpr auto YMM3   = ZYDIS_REGISTER_YMM3;
	static constexpr auto YMM4   = ZYDIS_REGISTER_YMM4;
	static constexpr auto YMM5   = ZYDIS_REGISTER_YMM5;
	static constexpr auto YMM6   = ZYDIS_REGISTER_YMM6;
	static constexpr auto YMM7   = ZYDIS_REGISTER_YMM7;
	static constexpr auto YMM8   = ZYDIS_REGISTER_YMM8;
	static constexpr auto YMM9   = ZYDIS_REGISTER_YMM9;
	static constexpr auto YMM10  = ZYDIS_REGISTER_YMM10;
	static constexpr auto YMM11  = ZYDIS_REGISTER_YMM11;
	static constexpr auto YMM12  = ZYDIS_REGISTER_YMM12;
	static constexpr auto YMM13  = ZYDIS_REGISTER_YMM13;
	static constexpr auto YMM14  = ZYDIS_REGISTER_YMM14;
	static constexpr auto YMM15  = ZYDIS_REGISTER_YMM15;
	static constexpr auto FLAGS  = ZYDIS_REGISTER_FLAGS;
	static constexpr auto EFLAGS = ZYDIS_REGISTER_EFLAGS;
	static constexpr auto RFLAGS = ZYDIS_REGISTER_RFLAGS;
	static constexpr auto IP     = ZYDIS_REGISTER_IP;
	static constexpr auto EIP    = ZYDIS_REGISTER_EIP;
	static constexpr auto RIP    = ZYDIS_REGISTER_RIP;

	// Encoding with explicit encoder request.
	//
	static bool encode(std::vector<uint8_t>& out, const ZydisEncoderRequest& req) {
		size_t pos = out.size();
		out.resize(pos + ZYDIS_MAX_INSTRUCTION_LENGTH);

		ZyanUSize instr_length = ZYDIS_MAX_INSTRUCTION_LENGTH;
		if (ZYAN_FAILED(ZydisEncoderEncodeInstruction(&req, out.data() + pos, &instr_length))) {
			return false;
		}
		assume_that(instr_length < ZYDIS_MAX_INSTRUCTION_LENGTH);
		out.resize(pos + instr_length);
		return true;
	}

	// Operand conversion.
	//
	struct mem {
		uint16_t      size  = 0;
		reg           base  = NO_REG;
		reg           index = NO_REG;
		uint8_t       scale = 0;
		int64_t       disp  = 0;
	};
	template<typename T>
	static ZydisEncoderOperand to_encoder_op(const T& op) {
		ZydisEncoderOperand res = {};

		if constexpr (std::is_same_v<T, ZydisEncoderOperand>) {
			return op;
		} else if constexpr (std::is_integral_v<T>) {
			res.type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
			if constexpr (std::is_unsigned_v<T>) {
				res.imm.u = op;
			} else {
				res.imm.s = op;
			}
		} else if constexpr (std::is_pointer_v<T>) {
			res.type  = ZYDIS_OPERAND_TYPE_IMMEDIATE;
			res.imm.u = (uintptr_t) op;
		} else if constexpr (std::is_same_v<T, reg>) {
			res.type      = ZYDIS_OPERAND_TYPE_REGISTER;
			res.reg.value = op;
		} else if constexpr (std::is_same_v<T, mem>) {
			res.type = ZYDIS_OPERAND_TYPE_MEMORY;
			res.mem  = {op.base, op.index, op.scale, op.disp, op.size};
		} else {
			static_assert(sizeof(T) == -1, "Invalid argument.");
		}
		return res;
	}

	// Free form encoding.
	//
	template<typename... Tx>
		requires(sizeof...(Tx) <= ZYDIS_ENCODER_MAX_OPERANDS)
	static bool encode(std::vector<uint8_t>& out, ZydisMnemonic mnemonic, const Tx&... operands) {
		ZydisEncoderRequest req;
		memset(&req, 0, sizeof(req));
		req.mnemonic                                                                  = mnemonic;
		req.machine_mode                                                              = ZYDIS_MACHINE_MODE_LONG_64;
		req.operand_count                                                             = sizeof...(Tx);
		((std::array<ZydisEncoderOperand, ZYDIS_ENCODER_MAX_OPERANDS>&) req.operands) = {to_encoder_op<Tx>(operands)...};
		return encode(out, req);
	}

	// Decoding.
	//
	struct decoded_ins {
		ZydisDecodedInstruction ins;
		ZydisDecodedOperand     ops[ZYDIS_MAX_OPERAND_COUNT_VISIBLE];

		// Formatting.
		//
		std::string to_string(uint64_t ip = 0) const {
			char           buffer[128];
			ZydisFormatter formatter;
			ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
			if (ZYAN_FAILED(ZydisFormatterFormatInstruction(&formatter, &ins, ops, ins.operand_count_visible, buffer, sizeof(buffer), ip))) {
				strcpy(buffer, "?");
			}
			return std::string(&buffer[0]);
		}
	};
	static std::optional<decoded_ins> decode(std::span<const uint8_t>& in) {
		std::optional<decoded_ins> result;
		ZydisDecoder               decoder;
		ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
		auto& out = result.emplace();
		if (ZYAN_FAILED(ZydisDecoderDecodeFull(&decoder, in.data(), in.size(), &out.ins, out.ops, ZYDIS_MAX_OPERAND_COUNT_VISIBLE, ZYDIS_DFLAG_VISIBLE_OPERANDS_ONLY))) {
			result.reset();
		} else {
			in = in.subspan(out.ins.length);
		}
		return result;
	}


	// Register resize map.
	//
	struct reg_details
	{
		reg gpr8lo = NO_REG;
		reg gpr8hi = NO_REG;
		reg gpr16  = NO_REG;
		reg gpr32  = NO_REG;
		reg gpr64  = NO_REG;
		reg gpr128 = NO_REG;
		reg gpr256 = NO_REG;
	};
	// ZYDIS_REGISTER_MAX_VALUE
	static constexpr auto reg_details_arr = []() {
		std::array<reg_details, ZYDIS_REGISTER_MAX_VALUE> arr = {};
		auto push_reg = [&](reg_details r) {
			if (r.gpr8lo != NO_REG)
				arr[r.gpr8lo] = r;
			if (r.gpr8hi != NO_REG)
				arr[r.gpr8hi] = r;
			if (r.gpr16 != NO_REG)
				arr[r.gpr16] = r;
			if (r.gpr32 != NO_REG)
				arr[r.gpr32] = r;
			if (r.gpr64 != NO_REG)
				arr[r.gpr64] = r;
			if (r.gpr128 != NO_REG)
				arr[r.gpr128] = r;
			if (r.gpr256 != NO_REG)
				arr[r.gpr256] = r;
		};
		push_reg({AL, AH, AX, EAX, RAX, NO_REG, NO_REG});
		push_reg({BL, BH, BX, EBX, RBX, NO_REG, NO_REG});
		push_reg({CL, CH, CX, ECX, RCX, NO_REG, NO_REG});
		push_reg({DL, DH, DX, EDX, RDX, NO_REG, NO_REG});
		push_reg({SPL, NO_REG, SP, ESP, RSP, NO_REG, NO_REG});
		push_reg({BPL, NO_REG, BP, EBP, RBP, NO_REG, NO_REG});
		push_reg({SIL, NO_REG, SI, ESI, RSI, NO_REG, NO_REG});
		push_reg({DIL, NO_REG, DI, EDI, RDI, NO_REG, NO_REG});
		push_reg({R8B, NO_REG, R8W, R8D, R8, NO_REG, NO_REG});
		push_reg({R9B, NO_REG, R9W, R9D, R9, NO_REG, NO_REG});
		push_reg({R10B, NO_REG, R10W, R10D, R10, NO_REG, NO_REG});
		push_reg({R11B, NO_REG, R11W, R11D, R11, NO_REG, NO_REG});
		push_reg({R12B, NO_REG, R12W, R12D, R12, NO_REG, NO_REG});
		push_reg({R13B, NO_REG, R13W, R13D, R13, NO_REG, NO_REG});
		push_reg({R14B, NO_REG, R14W, R14D, R14, NO_REG, NO_REG});
		push_reg({R15B, NO_REG, R15W, R15D, R15, NO_REG, NO_REG});

		push_reg({NO_REG, NO_REG, IP, EIP, RIP, NO_REG, NO_REG});
		push_reg({NO_REG, NO_REG, FLAGS, EFLAGS, RFLAGS, NO_REG, NO_REG});

		push_reg({NO_REG, NO_REG, NO_REG, NO_REG, NO_REG, XMM0, YMM0});
		push_reg({NO_REG, NO_REG, NO_REG, NO_REG, NO_REG, XMM1, YMM1});
		push_reg({NO_REG, NO_REG, NO_REG, NO_REG, NO_REG, XMM2, YMM2});
		push_reg({NO_REG, NO_REG, NO_REG, NO_REG, NO_REG, XMM3, YMM3});
		push_reg({NO_REG, NO_REG, NO_REG, NO_REG, NO_REG, XMM4, YMM4});
		push_reg({NO_REG, NO_REG, NO_REG, NO_REG, NO_REG, XMM5, YMM5});
		push_reg({NO_REG, NO_REG, NO_REG, NO_REG, NO_REG, XMM6, YMM6});
		push_reg({NO_REG, NO_REG, NO_REG, NO_REG, NO_REG, XMM7, YMM7});
		push_reg({NO_REG, NO_REG, NO_REG, NO_REG, NO_REG, XMM8, YMM8});
		push_reg({NO_REG, NO_REG, NO_REG, NO_REG, NO_REG, XMM9, YMM9});
		push_reg({NO_REG, NO_REG, NO_REG, NO_REG, NO_REG, XMM10, YMM10});
		push_reg({NO_REG, NO_REG, NO_REG, NO_REG, NO_REG, XMM11, YMM11});
		push_reg({NO_REG, NO_REG, NO_REG, NO_REG, NO_REG, XMM12, YMM12});
		push_reg({NO_REG, NO_REG, NO_REG, NO_REG, NO_REG, XMM13, YMM13});
		push_reg({NO_REG, NO_REG, NO_REG, NO_REG, NO_REG, XMM14, YMM14});
		push_reg({NO_REG, NO_REG, NO_REG, NO_REG, NO_REG, XMM15, YMM15});
		return arr;
	}();


	// Register resize.
	//
	static constexpr reg resize_reg(reg r, size_t n) {
		switch (n) {
			case 1:
				return reg_details_arr[r].gpr8lo;
			case 2:
				return reg_details_arr[r].gpr16;
			case 4:
				return reg_details_arr[r].gpr32;
			case 8:
				return reg_details_arr[r].gpr64;
			case 0x10:
				return reg_details_arr[r].gpr128;
			case 0x20:
				return reg_details_arr[r].gpr256;
			default:
				return NO_REG;
		}
	}
};