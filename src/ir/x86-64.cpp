#include <util/common.hpp>
#if LI_JIT && LI_ARCH_X86 && !LI_32
	#include <exception>
	#include <ir/ir2mir.hpp>
	#include <ir/x86-64.hpp>
	#include <iterator>
	#include <limits>
	#include <source_location>
	#include <string>
	#include <string_view>
	#include <unordered_map>

	#if LI_VTUNE
		#include <jitprofiling.h>
		#pragma comment(lib, "jitprofiling.lib")
	#endif

namespace li::ir {
	namespace {
		struct compile_error final : std::exception {
			std::string message;

			explicit compile_error(std::string reason = "x86 assembly rejected", std::source_location location = std::source_location::current())
				 : message(std::move(reason) + " [" + location.file_name() + ":" + std::to_string(location.line()) + "]") {}

			const char* what() const noexcept override { return message.c_str(); }
		};

		static std::string describe_encoder_failure(const ZydisEncoderRequest& request) {
			const char* mnemonic = ZydisMnemonicGetString(request.mnemonic);
			std::string result   = "x86 encoder rejected ";
			result += mnemonic ? mnemonic : ("mnemonic#" + std::to_string(unsigned(request.mnemonic)));
			if (request.operand_count)
				result += " operands=";
			for (uint8_t index = 0; index < request.operand_count; ++index) {
				if (index)
					result += ",";
				const auto& operand = request.operands[index];
				switch (operand.type) {
					case ZYDIS_OPERAND_TYPE_REGISTER: {
						const char* name = ZydisRegisterGetString(operand.reg.value);
						result += "reg:";
						result += name ? name : ("#" + std::to_string(unsigned(operand.reg.value)));
						result += "/" + std::to_string(ZydisRegisterGetWidth(request.machine_mode, operand.reg.value)) + "b";
						break;
					}
					case ZYDIS_OPERAND_TYPE_MEMORY:
						result += "mem/" + std::to_string(operand.mem.size) + "B";
						break;
					case ZYDIS_OPERAND_TYPE_POINTER:
						result += "ptr";
						break;
					case ZYDIS_OPERAND_TYPE_IMMEDIATE:
						result += "imm";
						break;
					default:
						result += "unused";
						break;
				}
			}
			return result;
		}

		static std::string code_memory_failure(std::string_view operation, const std::error_code& error) {
			return "x86 code memory " + std::string(operation) + " failed: " + error.message() + " (" + error.category().name() + ":" +
					 std::to_string(error.value()) + ")";
		}

		enum class relocation_kind : uint8_t {
			branch32,
		};
		struct relocation {
			relocation_kind kind;
			size_t          byte_offset;
			msize_t         target_block;
			int64_t         addend;
		};

		static constexpr zy::reg gp_registers[] = {
			 zy::RAX,
			 zy::RCX,
			 zy::RDX,
			 zy::RBX,
			 zy::RSP,
			 zy::RBP,
			 zy::RSI,
			 zy::RDI,
			 zy::R8,
			 zy::R9,
			 zy::R10,
			 zy::R11,
			 zy::R12,
			 zy::R13,
			 zy::R14,
			 zy::R15,
		};
		static constexpr zy::reg fp_registers[] = {
			 zy::XMM0,
			 zy::XMM1,
			 zy::XMM2,
			 zy::XMM3,
			 zy::XMM4,
			 zy::XMM5,
			 zy::XMM6,
			 zy::XMM7,
			 zy::XMM8,
			 zy::XMM9,
			 zy::XMM10,
			 zy::XMM11,
			 zy::XMM12,
			 zy::XMM13,
			 zy::XMM14,
			 zy::XMM15,
		};
		static constexpr zy::reg gp_scratch = zy::R11;
		static constexpr zy::reg fp_scratch = zy::XMM15;

		static zy::reg to_reg(mreg value, size_t size = 0) {
			if (!value)
				return zy::NO_REG;
			LI_ASSERT(value.is_phys());
			auto    physical = value.phys();
			auto    index    = arch::machine_id(physical);
			zy::reg result   = physical > 0 ? gp_registers[index] : fp_registers[index];
			if (size) {
				result = zy::resize_reg(result, size);
				if (result == zy::NO_REG)
					throw compile_error{};
			}
			return result;
		}

		static size_t integer_size(mwidth width) {
			switch (width) {
				case mwidth::i8:
					return 1;
				case mwidth::i16:
					return 2;
				case mwidth::i32:
					return 4;
				case mwidth::i64:
					return 8;
				default:
					throw compile_error{};
			}
		}
		static size_t scalar_size(mwidth width) {
			if (width == mwidth::f32)
				return 4;
			if (width == mwidth::f64)
				return 8;
			return integer_size(width);
		}

		static zy::mem to_mem(const mmem& memory, size_t size) {
			if (memory.base == mreg(vreg_cpool))
				throw compile_error{};
			return {
				 .size  = uint16_t(size),
				 .base  = to_reg(memory.base),
				 .index = to_reg(memory.index),
				 .scale = uint8_t(memory.index ? (uint8_t{1} << memory.shift) : 0),
				 .disp  = memory.disp,
			};
		}

		static void encode_or_throw(std::vector<uint8_t>& output, const ZydisEncoderRequest& request) {
			if (!zy::encode(output, request))
				throw compile_error{describe_encoder_failure(request)};
		}
		template<typename... Tx>
		static void encode_or_throw(std::vector<uint8_t>& output, ZydisMnemonic mnemonic, const Tx&... operands) {
			ZydisEncoderRequest request                                                       = {};
			request.mnemonic                                                                  = mnemonic;
			request.machine_mode                                                              = ZYDIS_MACHINE_MODE_LONG_64;
			request.operand_count                                                             = sizeof...(Tx);
			((std::array<ZydisEncoderOperand, ZYDIS_ENCODER_MAX_OPERANDS>&) request.operands) = {zy::to_encoder_op<Tx>(operands)...};
			encode_or_throw(output, request);
		}

		static void append_guarded(std::vector<uint8_t>& output, ZydisMnemonic jump_over, const std::vector<uint8_t>& body) {
			if (body.empty())
				return;
			encode_or_throw(output, jump_over, int64_t(body.size()));
			output.insert(output.end(), body.begin(), body.end());
		}

		class x86_assembler {
			mprocedure&             proc;
			std::vector<uint8_t>    assembly;
			std::vector<uint8_t>    epilogue;
			std::vector<relocation> relocations;
			size_t                  native_stack_requirement = 0;

			void mov_gp(zy::reg destination, const mop& source, size_t size = 8) {
				destination = zy::resize_reg(destination, size);
				if (destination == zy::NO_REG)
					throw compile_error{};
				if (source.is_reg()) {
					auto input = to_reg(source.reg, size);
					if (destination != input)
						encode_or_throw(assembly, ZYDIS_MNEMONIC_MOV, destination, input);
				} else if (source.is_const()) {
					if (source.i64 == 0) {
						auto destination32 = zy::resize_reg(destination, 4);
						encode_or_throw(assembly, ZYDIS_MNEMONIC_XOR, destination32, destination32);
					} else {
						encode_or_throw(assembly, ZYDIS_MNEMONIC_MOV, destination, source.i64);
					}
				} else if (source.is_mem()) {
					encode_or_throw(assembly, ZYDIS_MNEMONIC_MOV, destination, to_mem(source.mem, size));
				} else {
					throw compile_error{};
				}
			}

			void mov_fp(zy::reg destination, const mop& source, mwidth width) {
				if (source.is_reg()) {
					if (source.reg.is_fp()) {
						auto input = to_reg(source.reg);
						if (destination != input)
							encode_or_throw(assembly, ZYDIS_MNEMONIC_MOVAPS, destination, input);
					} else {
						auto input = to_reg(source.reg, width == mwidth::f32 ? 4 : 8);
						encode_or_throw(assembly, width == mwidth::f32 ? ZYDIS_MNEMONIC_MOVD : ZYDIS_MNEMONIC_MOVQ, destination, input);
					}
				} else if (source.is_const()) {
					auto size = width == mwidth::f32 ? 4 : 8;
					encode_or_throw(assembly, ZYDIS_MNEMONIC_MOV, zy::resize_reg(gp_scratch, size), source.i64);
					encode_or_throw(assembly, width == mwidth::f32 ? ZYDIS_MNEMONIC_MOVD : ZYDIS_MNEMONIC_MOVQ, destination, zy::resize_reg(gp_scratch, size));
				} else if (source.is_mem()) {
					encode_or_throw(
						 assembly, width == mwidth::f32 ? ZYDIS_MNEMONIC_MOVSS : ZYDIS_MNEMONIC_MOVSD, destination, to_mem(source.mem, scalar_size(width)));
				} else {
					throw compile_error{};
				}
			}

			void move_fp_to_gp(zy::reg destination, zy::reg source, mwidth width) {
				auto size = width == mwidth::f32 ? 4 : 8;
				encode_or_throw(assembly, width == mwidth::f32 ? ZYDIS_MNEMONIC_MOVD : ZYDIS_MNEMONIC_MOVQ, zy::resize_reg(destination, size), source);
			}

			void emit_branch32(ZydisMnemonic mnemonic, msize_t target_block, int64_t addend = 0) {
				ZydisEncoderRequest request = {};
				request.mnemonic            = mnemonic;
				request.machine_mode        = ZYDIS_MACHINE_MODE_LONG_64;
				request.branch_type         = ZYDIS_BRANCH_TYPE_NEAR;
				request.branch_width        = ZYDIS_BRANCH_WIDTH_32;
				request.operand_count       = 1;
				request.operands[0]         = zy::to_encoder_op(int32_t(0));
				size_t begin                = assembly.size();
				encode_or_throw(assembly, request);
				if (assembly.size() < begin + sizeof(int32_t))
					throw compile_error{};
				relocations.push_back({relocation_kind::branch32, assembly.size() - sizeof(int32_t), target_block, addend});
			}

			ZydisEncoderOperand gp_rhs(const mop& operand, size_t size) {
				if (operand.is_const() && size == 8 && operand.i64 != int32_t(operand.i64)) {
					encode_or_throw(assembly, ZYDIS_MNEMONIC_MOV, gp_scratch, operand.i64);
					return zy::to_encoder_op(gp_scratch);
				}
				if (operand.is_reg())
					return zy::to_encoder_op(to_reg(operand.reg, size));
				if (operand.is_mem())
					return zy::to_encoder_op(to_mem(operand.mem, size));
				return zy::to_encoder_op(operand.i64);
			}

			void integer_binary(const minsn& instruction, ZydisMnemonic mnemonic, bool commutative) {
				auto        size   = integer_size(instruction.width);
				auto        output = to_reg(instruction.out, size);
				const auto& lhs    = instruction.arg[0];
				const auto& rhs    = instruction.arg[1];
				if (mnemonic == ZYDIS_MNEMONIC_IMUL && rhs.is_const() && rhs.i64 == int32_t(rhs.i64)) {
					ZydisEncoderOperand source;
					if (lhs.is_const()) {
						mov_gp(gp_scratch, lhs, size);
						source = zy::to_encoder_op(zy::resize_reg(gp_scratch, size));
					} else {
						source = gp_rhs(lhs, size);
					}
					encode_or_throw(assembly, mnemonic, output, source, int32_t(rhs.i64));
					return;
				}
				if (lhs.is_reg() && to_reg(lhs.reg, size) == output) {
					encode_or_throw(assembly, mnemonic, output, gp_rhs(rhs, size));
					return;
				}
				if (commutative && rhs.is_reg() && to_reg(rhs.reg, size) == output) {
					encode_or_throw(assembly, mnemonic, output, gp_rhs(lhs, size));
					return;
				}
				if (rhs.is_reg() && to_reg(rhs.reg, size) == output) {
					mov_gp(gp_scratch, lhs, size);
					encode_or_throw(assembly, mnemonic, zy::resize_reg(gp_scratch, size), output);
					mov_gp(to_reg(instruction.out), mreg(arch::r11), size);
					return;
				}
				mov_gp(to_reg(instruction.out), lhs, size);
				encode_or_throw(assembly, mnemonic, output, gp_rhs(rhs, size));
			}

			void integer_unary(const minsn& instruction, ZydisMnemonic mnemonic) {
				auto size = integer_size(instruction.width);
				mov_gp(to_reg(instruction.out), instruction.arg[0], size);
				encode_or_throw(assembly, mnemonic, to_reg(instruction.out, size));
			}

			void integer_shift(const minsn& instruction, ZydisMnemonic mnemonic) {
				auto        size   = integer_size(instruction.width);
				auto        output = to_reg(instruction.out, size);
				const auto& count  = instruction.arg[1];
				if (count.is_const()) {
					mov_gp(to_reg(instruction.out), instruction.arg[0], size);
					encode_or_throw(assembly, mnemonic, output, uint8_t(count.i64));
					return;
				}
				if (!count.is_reg())
					throw compile_error{};

				// CL is the only variable-count shift input. Preserve an allocated RCX
				// and use R11 when RCX itself is the destination.
				encode_or_throw(assembly, ZYDIS_MNEMONIC_PUSH, zy::RCX);
				auto    count_register       = to_reg(count.reg);
				bool    output_aliases_count = output == zy::resize_reg(count_register, size);
				zy::reg work                 = output == zy::resize_reg(zy::RCX, size) || output_aliases_count ? zy::resize_reg(gp_scratch, size) : output;
				mov_gp(work, instruction.arg[0], size);
				if (count_register != zy::RCX)
					encode_or_throw(assembly, ZYDIS_MNEMONIC_MOV, zy::RCX, count_register);
				encode_or_throw(assembly, mnemonic, work, zy::CL);
				encode_or_throw(assembly, ZYDIS_MNEMONIC_POP, zy::RCX);
				if (work != output)
					encode_or_throw(assembly, ZYDIS_MNEMONIC_MOV, output, work);
			}

			void integer_divide(const minsn& instruction) {
				auto size = integer_size(instruction.width);
				if (size != 4 && size != 8)
					throw compile_error{};
				const auto& divisor = instruction.arg[1];
				if (divisor.is_reg())
					mov_gp(gp_scratch, divisor, size);
				else if (divisor.is_const())
					mov_gp(gp_scratch, divisor, size);
				else
					throw compile_error{};

				encode_or_throw(assembly, ZYDIS_MNEMONIC_PUSH, zy::RAX);
				encode_or_throw(assembly, ZYDIS_MNEMONIC_PUSH, zy::RDX);
				mov_gp(zy::RAX, instruction.arg[0], size);
				if (instruction.op == vop::iudiv) {
					encode_or_throw(assembly, ZYDIS_MNEMONIC_XOR, zy::EDX, zy::EDX);
				} else if (size == 8) {
					encode_or_throw(assembly, ZYDIS_MNEMONIC_CQO);
				} else {
					encode_or_throw(assembly, ZYDIS_MNEMONIC_CDQ);
				}
				encode_or_throw(assembly, instruction.op == vop::iudiv ? ZYDIS_MNEMONIC_DIV : ZYDIS_MNEMONIC_IDIV, zy::resize_reg(gp_scratch, size));
				auto result = instruction.op == vop::imod ? zy::resize_reg(zy::RDX, size) : zy::resize_reg(zy::RAX, size);
				encode_or_throw(assembly, ZYDIS_MNEMONIC_MOV, zy::resize_reg(gp_scratch, size), result);
				encode_or_throw(assembly, ZYDIS_MNEMONIC_POP, zy::RDX);
				encode_or_throw(assembly, ZYDIS_MNEMONIC_POP, zy::RAX);
				mov_gp(to_reg(instruction.out), mreg(arch::r11), size);
			}

			static ZydisMnemonic integer_setcc(cond condition) {
				switch (condition) {
					case cond::eq:
						return ZYDIS_MNEMONIC_SETZ;
					case cond::ne:
						return ZYDIS_MNEMONIC_SETNZ;
					case cond::slt:
						return ZYDIS_MNEMONIC_SETL;
					case cond::sle:
						return ZYDIS_MNEMONIC_SETLE;
					case cond::sgt:
						return ZYDIS_MNEMONIC_SETNLE;
					case cond::sge:
						return ZYDIS_MNEMONIC_SETNL;
					case cond::ult:
						return ZYDIS_MNEMONIC_SETB;
					case cond::ule:
						return ZYDIS_MNEMONIC_SETBE;
					case cond::ugt:
						return ZYDIS_MNEMONIC_SETNBE;
					case cond::uge:
						return ZYDIS_MNEMONIC_SETNB;
					default:
						throw compile_error{};
				}
			}

			void integer_compare(const minsn& instruction) {
				auto        size = integer_size(instruction.width);
				const auto& lhs  = instruction.arg[0];
				zy::reg     left;
				if (lhs.is_reg()) {
					left = to_reg(lhs.reg, size);
				} else {
					mov_gp(gp_scratch, lhs, size);
					left = zy::resize_reg(gp_scratch, size);
				}
				const auto& rhs = instruction.arg[1];
				if (left == zy::resize_reg(gp_scratch, size) && rhs.is_const() && size == 8 && rhs.i64 != int32_t(rhs.i64)) {
					encode_or_throw(assembly, ZYDIS_MNEMONIC_PUSH, zy::RAX);
					encode_or_throw(assembly, ZYDIS_MNEMONIC_MOV, zy::RAX, rhs.i64);
					encode_or_throw(assembly, ZYDIS_MNEMONIC_CMP, left, zy::RAX);
					encode_or_throw(assembly, ZYDIS_MNEMONIC_POP, zy::RAX);
				} else {
					encode_or_throw(assembly, ZYDIS_MNEMONIC_CMP, left, gp_rhs(rhs, size));
				}
				auto output  = to_reg(instruction.out);
				auto output8 = zy::resize_reg(output, 1);
				encode_or_throw(assembly, integer_setcc(cond(instruction.arg[2].i64)), output8);
				encode_or_throw(assembly, ZYDIS_MNEMONIC_MOVZX, zy::resize_reg(output, 4), output8);
			}

			void floating_binary(const minsn& instruction, ZydisMnemonic mnemonic, bool commutative) {
				auto        output      = to_reg(instruction.out);
				const auto& lhs_operand = instruction.arg[0];
				const auto& rhs_operand = instruction.arg[1];

				if (!rhs_operand.is_reg()) {
					mov_fp(output, lhs_operand, instruction.width);
					mov_fp(fp_scratch, rhs_operand, instruction.width);
					encode_or_throw(assembly, mnemonic, output, fp_scratch);
					return;
				}

				auto rhs = to_reg(rhs_operand.reg);
				if (!lhs_operand.is_reg()) {
					if (output == rhs) {
						mov_fp(fp_scratch, lhs_operand, instruction.width);
						encode_or_throw(assembly, mnemonic, fp_scratch, rhs);
						encode_or_throw(assembly, ZYDIS_MNEMONIC_MOVAPS, output, fp_scratch);
					} else {
						mov_fp(output, lhs_operand, instruction.width);
						encode_or_throw(assembly, mnemonic, output, rhs);
					}
					return;
				}

				auto lhs = to_reg(lhs_operand.reg);
				if (output == lhs) {
					encode_or_throw(assembly, mnemonic, output, rhs);
				} else if (commutative && output == rhs) {
					encode_or_throw(assembly, mnemonic, output, lhs);
				} else if (output == rhs) {
					encode_or_throw(assembly, ZYDIS_MNEMONIC_MOVAPS, fp_scratch, lhs);
					encode_or_throw(assembly, mnemonic, fp_scratch, rhs);
					encode_or_throw(assembly, ZYDIS_MNEMONIC_MOVAPS, output, fp_scratch);
				} else {
					encode_or_throw(assembly, ZYDIS_MNEMONIC_MOVAPS, output, lhs);
					encode_or_throw(assembly, mnemonic, output, rhs);
				}
			}

			void floating_sign_bit(const minsn& instruction, bool clear) {
				auto size   = instruction.width == mwidth::f32 ? 4 : 8;
				auto bit    = instruction.width == mwidth::f32 ? 31 : 63;
				auto input  = to_reg(instruction.arg[0].reg);
				auto output = to_reg(instruction.out);
				encode_or_throw(assembly, instruction.width == mwidth::f32 ? ZYDIS_MNEMONIC_MOVD : ZYDIS_MNEMONIC_MOVQ, zy::resize_reg(gp_scratch, size), input);
				encode_or_throw(assembly, clear ? ZYDIS_MNEMONIC_BTR : ZYDIS_MNEMONIC_BTC, zy::resize_reg(gp_scratch, size), uint8_t(bit));
				encode_or_throw(assembly, instruction.width == mwidth::f32 ? ZYDIS_MNEMONIC_MOVD : ZYDIS_MNEMONIC_MOVQ, output, zy::resize_reg(gp_scratch, size));
			}

			void floating_copysign(const minsn& instruction) {
				auto size   = instruction.width == mwidth::f32 ? 4 : 8;
				auto bit    = instruction.width == mwidth::f32 ? 31 : 63;
				auto move   = instruction.width == mwidth::f32 ? ZYDIS_MNEMONIC_MOVD : ZYDIS_MNEMONIC_MOVQ;
				auto output = to_reg(instruction.out);

				// Capture the sign before touching the output; output may alias either input.
				encode_or_throw(assembly, move, zy::resize_reg(gp_scratch, size), to_reg(instruction.arg[1].reg));
				encode_or_throw(assembly, ZYDIS_MNEMONIC_SHR, zy::resize_reg(gp_scratch, size), uint8_t(bit));
				encode_or_throw(assembly, ZYDIS_MNEMONIC_SHL, zy::resize_reg(gp_scratch, size), uint8_t(bit));
				encode_or_throw(assembly, move, fp_scratch, zy::resize_reg(gp_scratch, size));

				encode_or_throw(assembly, move, zy::resize_reg(gp_scratch, size), to_reg(instruction.arg[0].reg));
				encode_or_throw(assembly, ZYDIS_MNEMONIC_BTR, zy::resize_reg(gp_scratch, size), uint8_t(bit));
				encode_or_throw(assembly, move, output, zy::resize_reg(gp_scratch, size));
				encode_or_throw(assembly, instruction.width == mwidth::f32 ? ZYDIS_MNEMONIC_ORPS : ZYDIS_MNEMONIC_ORPD, output, fp_scratch);
			}

			void floating_minmax(const minsn& instruction) {
				auto lhs           = to_reg(instruction.arg[0].reg);
				auto rhs           = to_reg(instruction.arg[1].reg);
				auto scalar_minmax = instruction.op == vop::fmin ? (instruction.width == mwidth::f32 ? ZYDIS_MNEMONIC_MINSS : ZYDIS_MNEMONIC_MINSD)
																				 : (instruction.width == mwidth::f32 ? ZYDIS_MNEMONIC_MAXSS : ZYDIS_MNEMONIC_MAXSD);
				auto compare       = instruction.width == mwidth::f32 ? ZYDIS_MNEMONIC_UCOMISS : ZYDIS_MNEMONIC_UCOMISD;
				encode_or_throw(assembly, ZYDIS_MNEMONIC_MOVAPS, fp_scratch, lhs);
				encode_or_throw(assembly, scalar_minmax, fp_scratch, rhs);

				// x86 MIN/MAX returns the second operand for every unordered compare.
				// C fmin/fmax instead returns the numeric operand when exactly one is NaN.
				encode_or_throw(assembly, compare, rhs, rhs);
				std::vector<uint8_t> restore_lhs;
				encode_or_throw(restore_lhs, ZYDIS_MNEMONIC_MOVAPS, fp_scratch, lhs);
				append_guarded(assembly, ZYDIS_MNEMONIC_JNP, restore_lhs);

				// Equal zeros need deterministic IEEE signs: OR yields -0 for fmin,
				// AND yields +0 for fmax. Skip this correction for unordered inputs.
				encode_or_throw(assembly, compare, lhs, rhs);
				std::vector<uint8_t> combine_equal;
				encode_or_throw(combine_equal,
					 instruction.op == vop::fmin ? (instruction.width == mwidth::f32 ? ZYDIS_MNEMONIC_ORPS : ZYDIS_MNEMONIC_ORPD)
														  : (instruction.width == mwidth::f32 ? ZYDIS_MNEMONIC_ANDPS : ZYDIS_MNEMONIC_ANDPD),
					 fp_scratch, lhs);
				std::vector<uint8_t> equal_only;
				append_guarded(equal_only, ZYDIS_MNEMONIC_JNZ, combine_equal);
				append_guarded(assembly, ZYDIS_MNEMONIC_JP, equal_only);
				encode_or_throw(assembly, ZYDIS_MNEMONIC_MOVAPS, to_reg(instruction.out), fp_scratch);
			}

			void emit_setcc32(ZydisMnemonic mnemonic, zy::reg output) {
				auto byte = zy::resize_reg(output, 1);
				encode_or_throw(assembly, mnemonic, byte);
				encode_or_throw(assembly, ZYDIS_MNEMONIC_MOVZX, zy::resize_reg(output, 4), byte);
			}

			void floating_compare(const minsn& instruction) {
				auto lhs = to_reg(instruction.arg[0].reg);
				auto rhs = to_reg(instruction.arg[1].reg);
				encode_or_throw(assembly, instruction.width == mwidth::f32 ? ZYDIS_MNEMONIC_UCOMISS : ZYDIS_MNEMONIC_UCOMISD, lhs, rhs);
				auto output    = to_reg(instruction.out);
				auto condition = cond(instruction.arg[2].i64);
				switch (condition) {
					case cond::eq:
					case cond::slt:
					case cond::sle:
					case cond::ult:
					case cond::ule: {
						ZydisMnemonic relation = condition == cond::eq                                ? ZYDIS_MNEMONIC_SETZ
														 : (condition == cond::slt || condition == cond::ult) ? ZYDIS_MNEMONIC_SETB
																																: ZYDIS_MNEMONIC_SETBE;
						emit_setcc32(relation, output);
						emit_setcc32(ZYDIS_MNEMONIC_SETNP, gp_scratch);
						encode_or_throw(assembly, ZYDIS_MNEMONIC_AND, zy::resize_reg(output, 4), zy::R11D);
						break;
					}
					case cond::ne:
						emit_setcc32(ZYDIS_MNEMONIC_SETNZ, output);
						emit_setcc32(ZYDIS_MNEMONIC_SETP, gp_scratch);
						encode_or_throw(assembly, ZYDIS_MNEMONIC_OR, zy::resize_reg(output, 4), zy::R11D);
						break;
					case cond::sgt:
					case cond::ugt:
						emit_setcc32(ZYDIS_MNEMONIC_SETNBE, output);
						break;
					case cond::sge:
					case cond::uge:
						emit_setcc32(ZYDIS_MNEMONIC_SETNB, output);
						break;
					case cond::ordered:
						emit_setcc32(ZYDIS_MNEMONIC_SETNP, output);
						break;
					case cond::unordered:
						emit_setcc32(ZYDIS_MNEMONIC_SETP, output);
						break;
				}
			}

			void assemble_instruction(const minsn& instruction, msize_t next_block) {
				switch (instruction.op) {
					case vop::null:
						return;
					case vop::movi: {
						auto output = to_reg(instruction.out);
						if (instruction.arg[0].is_reg() && instruction.arg[0].reg.is_fp())
							move_fp_to_gp(output, to_reg(instruction.arg[0].reg), instruction.width == mwidth::i32 ? mwidth::f32 : mwidth::f64);
						else
							mov_gp(output, instruction.arg[0], integer_size(instruction.width));
						return;
					}
					case vop::movf:
						mov_fp(to_reg(instruction.out), instruction.arg[0], instruction.width);
						return;
					case vop::izx8:
					case vop::izx16: {
						auto source_size = instruction.op == vop::izx8 ? 1 : 2;
						auto output      = to_reg(instruction.out);
						encode_or_throw(assembly, ZYDIS_MNEMONIC_MOVZX, zy::resize_reg(output, 4), to_reg(instruction.arg[0].reg, source_size));
						return;
					}
					case vop::izx32:
						encode_or_throw(assembly, ZYDIS_MNEMONIC_MOV, to_reg(instruction.out, 4), to_reg(instruction.arg[0].reg, 4));
						return;
					case vop::isx8:
					case vop::isx16: {
						auto source_size = instruction.op == vop::isx8 ? 1 : 2;
						encode_or_throw(assembly, ZYDIS_MNEMONIC_MOVSX, to_reg(instruction.out), to_reg(instruction.arg[0].reg, source_size));
						return;
					}
					case vop::isx32:
						encode_or_throw(assembly, ZYDIS_MNEMONIC_MOVSXD, to_reg(instruction.out), to_reg(instruction.arg[0].reg, 4));
						return;
					case vop::fx32:
						encode_or_throw(assembly, ZYDIS_MNEMONIC_CVTSD2SS, to_reg(instruction.out), to_reg(instruction.arg[0].reg));
						return;
					case vop::fx64:
						encode_or_throw(assembly, ZYDIS_MNEMONIC_CVTSS2SD, to_reg(instruction.out), to_reg(instruction.arg[0].reg));
						return;
					case vop::fcvt: {
						auto source_size = instruction.width == mwidth::i64 ? 8 : 4;
						encode_or_throw(assembly, ZYDIS_MNEMONIC_CVTSI2SD, to_reg(instruction.out), to_reg(instruction.arg[0].reg, source_size));
						return;
					}
					case vop::icvt: {
						auto output           = to_reg(instruction.out);
						auto destination_size = instruction.width == mwidth::i64 ? 8 : 4;
						encode_or_throw(assembly, ZYDIS_MNEMONIC_CVTTSD2SI, zy::resize_reg(output, destination_size), to_reg(instruction.arg[0].reg));
						if (instruction.width == mwidth::i8)
							encode_or_throw(assembly, ZYDIS_MNEMONIC_MOVSX, output, zy::resize_reg(output, 1));
						else if (instruction.width == mwidth::i16)
							encode_or_throw(assembly, ZYDIS_MNEMONIC_MOVSX, output, zy::resize_reg(output, 2));
						else if (instruction.width == mwidth::i32)
							encode_or_throw(assembly, ZYDIS_MNEMONIC_MOVSXD, output, zy::resize_reg(output, 4));
						return;
					}

					case vop::loadi8:
					case vop::loadi16: {
						auto size = integer_size(instruction.width);
						encode_or_throw(assembly, ZYDIS_MNEMONIC_MOVZX, to_reg(instruction.out, 4), to_mem(instruction.arg[0].mem, size));
						return;
					}
					case vop::loadi32:
					case vop::loadi64: {
						auto size = integer_size(instruction.width);
						encode_or_throw(assembly, ZYDIS_MNEMONIC_MOV, to_reg(instruction.out, size), to_mem(instruction.arg[0].mem, size));
						return;
					}
					case vop::loadf32:
					case vop::loadf64:
						encode_or_throw(assembly, instruction.width == mwidth::f32 ? ZYDIS_MNEMONIC_MOVSS : ZYDIS_MNEMONIC_MOVSD, to_reg(instruction.out),
							 to_mem(instruction.arg[0].mem, scalar_size(instruction.width)));
						return;
					case vop::storei8:
					case vop::storei16:
					case vop::storei32:
					case vop::storei64: {
						auto size = integer_size(instruction.width);
						encode_or_throw(assembly, ZYDIS_MNEMONIC_MOV, to_mem(instruction.arg[0].mem, size), to_reg(instruction.arg[1].reg, size));
						return;
					}
					case vop::storef32:
					case vop::storef64:
						encode_or_throw(assembly, instruction.width == mwidth::f32 ? ZYDIS_MNEMONIC_MOVSS : ZYDIS_MNEMONIC_MOVSD,
							 to_mem(instruction.arg[0].mem, scalar_size(instruction.width)), to_reg(instruction.arg[1].reg));
						return;

					case vop::iadd:
						integer_binary(instruction, ZYDIS_MNEMONIC_ADD, true);
						return;
					case vop::isub:
						integer_binary(instruction, ZYDIS_MNEMONIC_SUB, false);
						return;
					case vop::imul:
						integer_binary(instruction, ZYDIS_MNEMONIC_IMUL, true);
						return;
					case vop::iand:
						integer_binary(instruction, ZYDIS_MNEMONIC_AND, true);
						return;
					case vop::ior:
						integer_binary(instruction, ZYDIS_MNEMONIC_OR, true);
						return;
					case vop::ixor:
						integer_binary(instruction, ZYDIS_MNEMONIC_XOR, true);
						return;
					case vop::ineg:
						integer_unary(instruction, ZYDIS_MNEMONIC_NEG);
						return;
					case vop::inot:
						integer_unary(instruction, ZYDIS_MNEMONIC_NOT);
						return;
					case vop::ishl:
						integer_shift(instruction, ZYDIS_MNEMONIC_SHL);
						return;
					case vop::ishr:
						integer_shift(instruction, ZYDIS_MNEMONIC_SHR);
						return;
					case vop::isar:
						integer_shift(instruction, ZYDIS_MNEMONIC_SAR);
						return;
					case vop::idiv:
					case vop::iudiv:
					case vop::imod:
						integer_divide(instruction);
						return;

					case vop::fadd:
						floating_binary(instruction, instruction.width == mwidth::f32 ? ZYDIS_MNEMONIC_ADDSS : ZYDIS_MNEMONIC_ADDSD, true);
						return;
					case vop::fsub:
						floating_binary(instruction, instruction.width == mwidth::f32 ? ZYDIS_MNEMONIC_SUBSS : ZYDIS_MNEMONIC_SUBSD, false);
						return;
					case vop::fmul:
						floating_binary(instruction, instruction.width == mwidth::f32 ? ZYDIS_MNEMONIC_MULSS : ZYDIS_MNEMONIC_MULSD, true);
						return;
					case vop::fdiv:
						floating_binary(instruction, instruction.width == mwidth::f32 ? ZYDIS_MNEMONIC_DIVSS : ZYDIS_MNEMONIC_DIVSD, false);
						return;
					case vop::fneg:
						floating_sign_bit(instruction, false);
						return;
					case vop::fabs:
						floating_sign_bit(instruction, true);
						return;
					case vop::fsqrt:
						encode_or_throw(assembly, instruction.width == mwidth::f32 ? ZYDIS_MNEMONIC_SQRTSS : ZYDIS_MNEMONIC_SQRTSD, to_reg(instruction.out),
							 to_reg(instruction.arg[0].reg));
						return;
					case vop::fround: {
						uint8_t mode;
						switch (round_mode(instruction.arg[1].i64)) {
							case round_mode::nearest:
								mode = 0;
								break;
							case round_mode::down:
								mode = 1;
								break;
							case round_mode::up:
								mode = 2;
								break;
							case round_mode::toward_zero:
								mode = 3;
								break;
						}
						encode_or_throw(assembly, instruction.width == mwidth::f32 ? ZYDIS_MNEMONIC_ROUNDSS : ZYDIS_MNEMONIC_ROUNDSD, to_reg(instruction.out),
							 to_reg(instruction.arg[0].reg), uint8_t(mode | 8));
						return;
					}
					case vop::fmin:
					case vop::fmax:
						floating_minmax(instruction);
						return;
					case vop::fcopysign:
						floating_copysign(instruction);
						return;
					case vop::icmp:
						integer_compare(instruction);
						return;
					case vop::fcmp:
						floating_compare(instruction);
						return;
					case vop::lea:
						encode_or_throw(assembly, ZYDIS_MNEMONIC_LEA, to_reg(instruction.out), to_mem(instruction.arg[0].mem, 8));
						return;
					case vop::crc32:
						integer_binary(instruction, ZYDIS_MNEMONIC_CRC32, false);
						return;
					case vop::rdcycle: {
						encode_or_throw(assembly, ZYDIS_MNEMONIC_PUSH, zy::RAX);
						encode_or_throw(assembly, ZYDIS_MNEMONIC_PUSH, zy::RDX);
						encode_or_throw(assembly, ZYDIS_MNEMONIC_RDTSC);
						encode_or_throw(assembly, ZYDIS_MNEMONIC_SHL, zy::RDX, uint8_t(32));
						encode_or_throw(assembly, ZYDIS_MNEMONIC_OR, zy::RAX, zy::RDX);
						encode_or_throw(assembly, ZYDIS_MNEMONIC_MOV, gp_scratch, zy::RAX);
						encode_or_throw(assembly, ZYDIS_MNEMONIC_POP, zy::RDX);
						encode_or_throw(assembly, ZYDIS_MNEMONIC_POP, zy::RAX);
						mov_gp(to_reg(instruction.out), mreg(arch::r11));
						return;
					}

					case vop::select: {
						auto condition = to_reg(instruction.arg[0].reg, 1);
						encode_or_throw(assembly, ZYDIS_MNEMONIC_TEST, condition, condition);
						if (instruction.out.is_gp()) {
							mov_gp(gp_scratch, instruction.arg[2], integer_size(instruction.width));
							encode_or_throw(assembly, ZYDIS_MNEMONIC_CMOVNZ, zy::resize_reg(gp_scratch, integer_size(instruction.width)),
								 to_reg(instruction.arg[1].reg, integer_size(instruction.width)));
							mov_gp(to_reg(instruction.out), mreg(arch::r11), integer_size(instruction.width));
						} else {
							encode_or_throw(assembly, ZYDIS_MNEMONIC_MOVAPS, fp_scratch, to_reg(instruction.arg[2].reg));
							std::vector<uint8_t> true_move;
							encode_or_throw(true_move, ZYDIS_MNEMONIC_MOVAPS, fp_scratch, to_reg(instruction.arg[1].reg));
							append_guarded(assembly, ZYDIS_MNEMONIC_JZ, true_move);
							encode_or_throw(assembly, ZYDIS_MNEMONIC_MOVAPS, to_reg(instruction.out), fp_scratch);
						}
						return;
					}
					case vop::call: {
						if (instruction.arg[0].is_const()) {
							encode_or_throw(assembly, ZYDIS_MNEMONIC_MOV, gp_scratch, instruction.arg[0].i64);
							encode_or_throw(assembly, ZYDIS_MNEMONIC_CALL, gp_scratch);
						} else if (instruction.arg[0].is_reg()) {
							encode_or_throw(assembly, ZYDIS_MNEMONIC_CALL, to_reg(instruction.arg[0].reg));
						} else {
							throw compile_error{};
						}
						return;
					}
					case vop::js: {
						auto condition = to_reg(instruction.arg[0].reg, 1);
						encode_or_throw(assembly, ZYDIS_MNEMONIC_TEST, condition, condition);
						auto true_block  = msize_t(instruction.arg[1].i64);
						auto false_block = msize_t(instruction.arg[2].i64);
						if (false_block == next_block) {
							emit_branch32(ZYDIS_MNEMONIC_JNZ, true_block);
						} else if (true_block == next_block) {
							emit_branch32(ZYDIS_MNEMONIC_JZ, false_block);
						} else {
							emit_branch32(ZYDIS_MNEMONIC_JNZ, true_block);
							emit_branch32(ZYDIS_MNEMONIC_JMP, false_block);
						}
						return;
					}
					case vop::jmp:
						if (msize_t(instruction.arg[0].i64) != next_block)
							emit_branch32(ZYDIS_MNEMONIC_JMP, msize_t(instruction.arg[0].i64));
						return;
					case vop::ret:
						LI_ASSERT(instruction.arg[0].reg == mreg(arch::gp_retval));
						assembly.insert(assembly.end(), epilogue.begin(), epilogue.end());
						encode_or_throw(assembly, ZYDIS_MNEMONIC_RET);
						return;
					case vop::unreachable:
						encode_or_throw(assembly, ZYDIS_MNEMONIC_UD2);
						return;
				}
				throw compile_error{};
			}

			void build_frame() {
				assembly.push_back(0x90);  // stable one-byte breakpoint site

				std::vector<arch::reg> saved_gp;
				for (auto reg : arch::gp_nonvolatile) {
					if (proc.used_gp_mask & arch::mask_of(reg)) {
						saved_gp.push_back(reg);
						encode_or_throw(assembly, ZYDIS_MNEMONIC_PUSH, to_reg(mreg(reg)));
					}
				}

				std::vector<arch::reg> saved_fp;
				for (auto reg : arch::fp_nonvolatile) {
					if (proc.used_fp_mask & arch::mask_of(reg))
						saved_fp.push_back(reg);
				}
	#if LI_ABI_MS64
				// XMM15 is reserved selector scratch and nonvolatile on Win64.
				saved_fp.push_back(arch::xmm15);
	#endif

				auto outgoing_bytes = (size_t(proc.used_stack_length) + arch::stack_alignment - 1) & ~(arch::stack_alignment - 1);
				auto save_bytes     = saved_fp.size() * 16;
				auto frame_bytes    = outgoing_bytes + save_bytes;
				auto entry_mod      = size_t(8 - 8 * saved_gp.size()) & (arch::stack_alignment - 1);
				auto padding        = (entry_mod - frame_bytes) & (arch::stack_alignment - 1);
				auto allocation     = frame_bytes + padding;

				// CALL has already pushed its return address. The generated prologue
				// then pushes every GP callee-save and reserves one aligned area for
				// outgoing arguments, allocator spills, and FP callee-saves.
				native_stack_requirement = sizeof(uintptr_t) + saved_gp.size() * sizeof(uint64_t) + allocation;
				if (allocation)
					encode_or_throw(assembly, ZYDIS_MNEMONIC_SUB, zy::RSP, allocation);

				intptr_t save_cursor = intptr_t(outgoing_bytes + save_bytes);
				for (auto reg : saved_fp) {
					save_cursor -= 16;
					encode_or_throw(assembly, ZYDIS_MNEMONIC_MOVAPS, zy::mem{.size = 16, .base = zy::RSP, .disp = save_cursor}, to_reg(mreg(reg)));
				}

				for (auto it = saved_fp.rbegin(); it != saved_fp.rend(); ++it) {
					encode_or_throw(epilogue, ZYDIS_MNEMONIC_MOVAPS, to_reg(mreg(*it)), zy::mem{.size = 16, .base = zy::RSP, .disp = save_cursor});
					save_cursor += 16;
				}
				if (allocation)
					encode_or_throw(epilogue, ZYDIS_MNEMONIC_ADD, zy::RSP, allocation);
				for (auto it = saved_gp.rbegin(); it != saved_gp.rend(); ++it)
					encode_or_throw(epilogue, ZYDIS_MNEMONIC_POP, to_reg(mreg(*it)));
			}

		  public:
			explicit x86_assembler(mprocedure& procedure) : proc(procedure) {}

			jfunction* assemble() {
				proc.basic_blocks.sort([](const mblock& lhs, const mblock& rhs) { return lhs.uid < rhs.uid; });
				build_frame();
				for (auto block_it = proc.basic_blocks.begin(); block_it != proc.basic_blocks.end(); ++block_it) {
					auto& block      = *block_it;
					auto  next_it    = std::next(block_it);
					auto  next_block = next_it == proc.basic_blocks.end() ? std::numeric_limits<msize_t>::max() : next_it->uid;
					block.asm_loc    = assembly.size();
					for (const auto& instruction : block.instructions)
						assemble_instruction(instruction, next_block);
				}

				std::unordered_map<msize_t, size_t> block_offsets;
				for (const auto& block : proc.basic_blocks)
					block_offsets.emplace(block.uid, block.asm_loc);
				for (const auto& entry : relocations) {
					if (entry.kind != relocation_kind::branch32 || entry.byte_offset > assembly.size() || assembly.size() - entry.byte_offset < sizeof(int32_t))
						throw compile_error{"invalid x86 branch32 relocation at byte " + std::to_string(entry.byte_offset)};
					auto target = block_offsets.find(entry.target_block);
					if (target == block_offsets.end())
						throw compile_error{"x86 branch32 relocation targets missing block " + std::to_string(entry.target_block)};
					intptr_t displacement = intptr_t(target->second) + entry.addend - intptr_t(entry.byte_offset + sizeof(int32_t));
					if (int32_t(displacement) != displacement)
						throw compile_error{"x86 branch32 relocation displacement out of range: " + std::to_string(displacement)};
					int32_t value = int32_t(displacement);
					memcpy(assembly.data() + entry.byte_offset, &value, sizeof(value));
				}

				if (assembly.empty())
					throw compile_error{};
				std::error_code error;
				auto            memory = platform::code_memory::allocate(assembly.size(), error);
				if (error)
					throw compile_error{code_memory_failure("allocation", error)};
				if (error = memory.write(0, std::as_bytes(std::span(assembly))); error)
					throw compile_error{code_memory_failure("write", error)};
				if (error = memory.publish(); error)
					throw compile_error{code_memory_failure("publication", error)};
				auto* result = jfunction::create(proc.source->L, proc.source->f, std::move(memory),
					 jfunction::layout{
						  .code_size          = assembly.size(),
						  .native_stack_bytes = native_stack_requirement,
						  .vm_stack_slots     = size_t(proc.source->f->num_locals) + size_t(proc.source->max_stack_slot),
						  .osr_target         = proc.source->osr_target,
					 },
					 proc.const_pool, std::move(proc.source->inline_caches));

	#if LI_VTUNE
				iJIT_Method_Load event    = {};
				event.method_id           = iJIT_GetNewMethodID();
				event.method_load_address = const_cast<void*>(result->entry_address());
				event.method_size         = uint32_t(assembly.size());
				event.method_name         = (char*) "Lightning JIT Code";
				result->uid               = event.method_id;
				iJIT_NotifyEvent(iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED, &event);
	#endif
				return result;
			}
		};
	}

	jfunction* assemble_ir(mprocedure* procedure) {
		if (!procedure)
			return nullptr;
		procedure->assembly_error.clear();
		if (!procedure->source || !procedure->source->L || !procedure->source->f) {
			if (procedure->source)
				procedure->source->inline_caches.reset();
			procedure->assembly_error = "x86 assembly requires a procedure with VM and function owners";
			return nullptr;
		}
		try {
			return x86_assembler(*procedure).assemble();
		} catch (const std::exception& error) {
			procedure->source->inline_caches.reset();
			procedure->assembly_error = error.what();
			return nullptr;
		} catch (...) {
			procedure->source->inline_caches.reset();
			procedure->assembly_error = "x86 assembly failed with a non-standard exception";
			return nullptr;
		}
	}

	std::string disassemble_code(const jfunction& function) {
		std::string result;
		auto        bytes   = function.code_bytes();
		auto        input   = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
		uintptr_t   address = reinterpret_cast<uintptr_t>(function.entry_address());
		while (!input.empty()) {
			auto before  = input.size();
			auto decoded = zy::decode(input);
			if (!decoded) {
				result += util::fmt("db 0x%02x\n", input.front());
				input = input.subspan(1);
				address++;
				continue;
			}
			result += decoded->to_string(address);
			result += '\n';
			address += before - input.size();
		}
		return result;
	}
}

#endif
