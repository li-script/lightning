#pragma once
#include <util/common.hpp>
#if !LI_ARCH_X86 || LI_32
	#error "JIT mode is only available for x86-64."
#endif
#include <span>
#include <vector>
#include <string>
#include <optional>

#if _WIN32
	#define NOMINMAX // athre0z y u include windows :(
	#define WIN32_LEAN_AND_MEAN
#endif
#include <Zycore/API/Memory.h>
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
	static constexpr auto EFLAGS = ZYDIS_REGISTER_EFLAGS;
	static constexpr auto RFLAGS = ZYDIS_REGISTER_RFLAGS;
	static constexpr auto RIP    = ZYDIS_REGISTER_RIP;
	static constexpr auto ES     = ZYDIS_REGISTER_ES;
	static constexpr auto CS     = ZYDIS_REGISTER_CS;
	static constexpr auto SS     = ZYDIS_REGISTER_SS;
	static constexpr auto DS     = ZYDIS_REGISTER_DS;
	static constexpr auto FS     = ZYDIS_REGISTER_FS;
	static constexpr auto GS     = ZYDIS_REGISTER_GS;

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

		if constexpr (std::is_integral_v<T>) {
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
};