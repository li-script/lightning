#pragma once
#if !LI_ARCH_X86 || LI_32
	#error "JIT mode is only available for x86-64."
#endif

#include <Zycore/API/Memory.h>
#include <Zycore/LibC.h>
#include <Zydis/Zydis.h>
#include <span>
#include <util/common.hpp>
#include <vector>
namespace li::zy {
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
		ZydisRegister base  = ZYDIS_REGISTER_NONE;
		ZydisRegister index = ZYDIS_REGISTER_NONE;
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
		} else if constexpr (std::is_same_v<T, ZydisRegister>) {
			res.type      = ZYDIS_OPERAND_TYPE_REGISTER;
			res.reg.value = op;
		} else if constexpr (std::is_same_v<T, mem>) {
			res.type = ZYDIS_OPERAND_TYPE_MEMORY;
			res.mem  = {op.base, op.index, op.scale, op.disp, op.size};
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