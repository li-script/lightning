#include <util/common.hpp>
#if LI_JIT && LI_ARCH_ARM && !LI_32
	#include <ir/a64.hpp>
	#include <ir/ir2mir.hpp>
	#include <ir/mir.hpp>
	#include <vm/function.hpp>

	#include <algorithm>
	#include <array>
	#include <bit>
	#include <cmath>
	#include <cstddef>
	#include <cstdint>
	#include <cstring>
	#include <iomanip>
	#include <limits>
	#include <source_location>
	#include <sstream>
	#include <stdexcept>
	#include <string>
	#include <string_view>
	#include <utility>
	#include <vector>

	#if defined(__APPLE__)
		#include <sys/sysctl.h>
	#elif defined(__linux__)
		#include <asm/hwcap.h>
		#include <sys/auxv.h>
	#endif

	#if LI_VTUNE
		#include <jitprofiling.h>
	#endif

namespace li::ir {
	namespace {
		using a64::condition;
		using a64::emitter;
		using a64::fp_reg;
		using a64::fp_round;
		using a64::fp_width;
		using a64::gp_reg;
		using a64::gp_width;
		using a64::memory_size;
		using a64::pair_mode;
		using a64::shift;

		constexpr gp_reg scratch0 = 16;
		constexpr gp_reg scratch1 = 17;
		constexpr fp_reg fscratch = 31;

		struct compile_error final : std::exception {
			std::string message;

			explicit compile_error(std::string reason = "A64 assembly rejected", std::source_location location = std::source_location::current())
				 : message(std::move(reason) + " [" + location.file_name() + ":" + std::to_string(location.line()) + "]") {}

			const char* what() const noexcept override { return message.c_str(); }
		};

		static std::string code_memory_failure(std::string_view operation, const std::error_code& error) {
			return "A64 code memory " + std::string(operation) + " failed: " + error.message() + " (" + error.category().name() + ":" +
					 std::to_string(error.value()) + ")";
		}

		constexpr gp_reg gp_number(mreg value) {
			if (!value.is_phys() || !value.is_gp())
				throw compile_error{};
			return arch::reg_number(value.phys());
		}
		constexpr fp_reg fp_number(mreg value) {
			if (!value.is_phys() || !value.is_fp())
				throw compile_error{};
			return arch::reg_number(value.phys());
		}

		constexpr gp_width integer_width(mwidth width) { return width == mwidth::i64 ? gp_width::x64 : gp_width::w32; }
		constexpr fp_width floating_width(mwidth width) { return width == mwidth::f32 ? fp_width::s32 : fp_width::d64; }
		constexpr uint32_t memory_bytes(memory_size width) { return uint32_t{1} << static_cast<uint8_t>(width); }

		constexpr bool add_immediate(int64_t value, uint32_t& immediate, uint32_t& amount) {
			uint64_t magnitude = value < 0 ? uint64_t(-(value + 1)) + 1 : uint64_t(value);
			if (magnitude <= 4095) {
				immediate = uint32_t(magnitude);
				amount    = 0;
				return true;
			}
			if ((magnitude & 0xfffu) == 0 && (magnitude >> 12) <= 4095) {
				immediate = uint32_t(magnitude >> 12);
				amount    = 12;
				return true;
			}
			return false;
		}

		constexpr bool direct_memory_offset(int32_t displacement, uint32_t size) {
			return (displacement >= 0 && displacement % int32_t(size) == 0 && uint32_t(displacement / int32_t(size)) <= 4095) ||
					 (displacement >= -256 && displacement <= 255);
		}

		bool cpu_has_crc32() {
			static const bool supported = [] {
	#if defined(__APPLE__)
				int    value = 0;
				size_t size  = sizeof(value);
				return sysctlbyname("hw.optional.armv8_crc32", &value, &size, nullptr, 0) == 0 && value != 0;
	#elif defined(__linux__) && defined(HWCAP_CRC32)
				return (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0;
	#elif defined(__ARM_FEATURE_CRC32)
				return true;
	#else
				return false;
	#endif
			}();
			return supported;
		}

		condition integer_condition(cond predicate) {
			switch (predicate) {
				case cond::eq:
					return condition::eq;
				case cond::ne:
					return condition::ne;
				case cond::slt:
					return condition::lt;
				case cond::sle:
					return condition::le;
				case cond::sgt:
					return condition::gt;
				case cond::sge:
					return condition::ge;
				case cond::ult:
					return condition::lo;
				case cond::ule:
					return condition::ls;
				case cond::ugt:
					return condition::hi;
				case cond::uge:
					return condition::hs;
				default:
					throw compile_error{};
			}
		}

		condition floating_condition(cond predicate) {
			// FCMP reports unordered as NZCV=0011. These predicates therefore make
			// every relation ordered except NE, whose language contract includes NaN.
			switch (predicate) {
				case cond::eq:
					return condition::eq;
				case cond::ne:
					return condition::ne;
				case cond::slt:
				case cond::ult:
					return condition::mi;
				case cond::sle:
				case cond::ule:
					return condition::ls;
				case cond::sgt:
				case cond::ugt:
					return condition::gt;
				case cond::sge:
				case cond::uge:
					return condition::ge;
				case cond::ordered:
					return condition::vc;
				case cond::unordered:
					return condition::vs;
			}
			throw compile_error{};
		}

		class assembler {
			mprocedure&                                        proc;
			emitter                                            code;
			std::vector<emitter::label>                        block_labels;
			std::vector<std::pair<uint64_t, emitter::literal>> literals64;
			std::vector<std::pair<uint32_t, emitter::literal>> literals32;
			size_t                                             frame_size = 0;
			size_t                                             save_begin = 0;

			emitter::literal literal64(uint64_t value) {
				for (const auto& [candidate, literal] : literals64) {
					if (candidate == value)
						return literal;
				}
				auto result = code.literal64(value);
				literals64.emplace_back(value, result);
				return result;
			}
			emitter::literal literal32(uint32_t value) {
				for (const auto& [candidate, literal] : literals32) {
					if (candidate == value)
						return literal;
				}
				auto result = code.literal32(value);
				literals32.emplace_back(value, result);
				return result;
			}

			uint64_t constant_pool_value(const mmem& memory) const {
				if (memory.index || memory.disp < 0 || (memory.disp % int32_t(sizeof(any))) != 0)
					throw compile_error{};
				size_t index = size_t(memory.disp) / sizeof(any);
				if (index >= proc.const_pool.size())
					throw compile_error{};
				return proc.const_pool[index].value;
			}

			gp_reg materialize(const mop& operand, gp_reg scratch, gp_width width) {
				if (operand.is_reg())
					return gp_number(operand.reg);
				if (!operand.is_const())
					throw compile_error{};
				uint64_t value = uint64_t(operand.i64);
				if (width == gp_width::w32)
					value = uint32_t(value);
				code.mov_constant(width, scratch, value);
				return scratch;
			}

			void emit_sp_adjust(bool subtract, size_t amount) {
				// Shifted-register ADD/SUB interprets register 31 as ZR, not SP.
				// Split arbitrary frame adjustments into legal ADD/SUB-immediate chunks.
				while (amount) {
					uint32_t immediate;
					uint32_t amount_shift;
					size_t   chunk;
					if (amount >= 4096) {
						immediate    = uint32_t(std::min(amount >> 12, size_t(4095)));
						amount_shift = 12;
						chunk        = size_t(immediate) << 12;
					} else {
						immediate    = uint32_t(amount);
						amount_shift = 0;
						chunk        = amount;
					}
					if (subtract)
						code.sub_imm(gp_width::x64, 31, 31, immediate, amount_shift);
					else
						code.add_imm(gp_width::x64, 31, 31, immediate, amount_shift);
					amount -= chunk;
				}
			}

			gp_reg stack_address(size_t offset) {
				code.add_imm(gp_width::x64, scratch1, 31, 0);
				while (offset) {
					uint32_t immediate;
					uint32_t amount_shift;
					size_t   chunk;
					if (offset >= 4096) {
						immediate    = uint32_t(std::min(offset >> 12, size_t(4095)));
						amount_shift = 12;
						chunk        = size_t(immediate) << 12;
					} else {
						immediate    = uint32_t(offset);
						amount_shift = 0;
						chunk        = offset;
					}
					code.add_imm(gp_width::x64, scratch1, scratch1, immediate, amount_shift);
					offset -= chunk;
				}
				return scratch1;
			}

			void save_gp(gp_reg value, size_t offset, bool load) {
				if (offset <= uint64_t(std::numeric_limits<int32_t>::max()) && direct_memory_offset(int32_t(offset), 8)) {
					if (load)
						code.ldr(memory_size::u64, value, 31, int32_t(offset));
					else
						code.str(memory_size::u64, value, 31, int32_t(offset));
				} else {
					gp_reg address = stack_address(offset);
					if (load)
						code.ldr(memory_size::u64, value, address);
					else
						code.str(memory_size::u64, value, address);
				}
			}

			void save_fp(fp_reg value, size_t offset, bool load) {
				if (offset <= uint64_t(std::numeric_limits<int32_t>::max()) && direct_memory_offset(int32_t(offset), 8)) {
					if (load)
						code.ldr(fp_width::d64, value, 31, int32_t(offset));
					else
						code.str(fp_width::d64, value, 31, int32_t(offset));
				} else {
					gp_reg address = stack_address(offset);
					if (load)
						code.ldr(fp_width::d64, value, address);
					else
						code.str(fp_width::d64, value, address);
				}
			}

			void prologue() {
				code.stp(gp_width::x64, 29, 30, 31, -16, pair_mode::pre_index);
				code.add_imm(gp_width::x64, 29, 31, 0);

				size_t spill_bytes = proc.used_stack_length < 0 ? 0 : size_t(proc.used_stack_length);
				save_begin         = (spill_bytes + 15) & ~size_t(15);
				size_t saved_count = 0;
				for (arch::reg value : arch::gp_nonvolatile)
					saved_count += (proc.used_gp_mask & arch::reg_mask(value)) != 0;
				for (arch::reg value : arch::fp_nonvolatile)
					saved_count += (proc.used_fp_mask & arch::reg_mask(value)) != 0;
				if (saved_count > (std::numeric_limits<size_t>::max() - save_begin) / 8)
					throw compile_error{};
				frame_size = (save_begin + saved_count * 8 + 15) & ~size_t(15);
				emit_sp_adjust(true, frame_size);

				size_t offset = save_begin;
				for (arch::reg value : arch::gp_nonvolatile) {
					if (proc.used_gp_mask & arch::reg_mask(value)) {
						save_gp(arch::reg_number(value), offset, false);
						offset += 8;
					}
				}
				// AAPCS64 preserves only the low 64-bit D lanes of V8-V15.
				for (arch::reg value : arch::fp_nonvolatile) {
					if (proc.used_fp_mask & arch::reg_mask(value)) {
						save_fp(arch::reg_number(value), offset, false);
						offset += 8;
					}
				}
			}

			void epilogue() {
				size_t offset = save_begin;
				for (arch::reg value : arch::gp_nonvolatile) {
					if (proc.used_gp_mask & arch::reg_mask(value)) {
						save_gp(arch::reg_number(value), offset, true);
						offset += 8;
					}
				}
				for (arch::reg value : arch::fp_nonvolatile) {
					if (proc.used_fp_mask & arch::reg_mask(value)) {
						save_fp(arch::reg_number(value), offset, true);
						offset += 8;
					}
				}
				emit_sp_adjust(false, frame_size);
				code.ldp(gp_width::x64, 29, 30, 31, 16, pair_mode::post_index);
				code.ret();
			}

			struct address {
				gp_reg  base;
				int32_t displacement;
			};

			address legalize_address(const mmem& memory, uint32_t access_size) {
				if (!memory.base || memory.base == mreg(vreg_cpool))
					throw compile_error{};
				gp_reg base = gp_number(memory.base);
				if (memory.index) {
					if (base == 31) {
						code.add_imm(gp_width::x64, scratch1, 31, 0);
						base = scratch1;
					}
					code.add(gp_width::x64, scratch1, base, gp_number(memory.index), shift::lsl, memory.shift);
					base = scratch1;
				}
				if (direct_memory_offset(memory.disp, access_size))
					return {base, memory.disp};
				code.mov_constant(gp_width::x64, scratch0, uint64_t(int64_t(memory.disp)));
				if (base == 31) {
					code.add_imm(gp_width::x64, scratch1, 31, 0);
					base = scratch1;
				}
				code.add(gp_width::x64, scratch1, base, scratch0);
				return {scratch1, 0};
			}

			void emit_integer_load(const minsn& instruction, memory_size width) {
				if (!instruction.arg[0].is_mem())
					throw compile_error{};
				const mmem& memory = instruction.arg[0].mem;
				gp_reg      out    = gp_number(instruction.out);
				if (memory.base == mreg(vreg_cpool)) {
					uint64_t value = constant_pool_value(memory);
					if (width == memory_size::u64)
						code.ldr_literal(gp_width::x64, out, literal64(value));
					else if (width == memory_size::u32)
						code.ldr_literal(gp_width::w32, out, literal32(uint32_t(value)));
					else
						code.mov_constant(gp_width::w32, out, uint32_t(value) & (width == memory_size::u8 ? 0xffu : 0xffffu));
					return;
				}
				auto source = legalize_address(memory, memory_bytes(width));
				code.ldr(width, out, source.base, source.displacement);
			}

			void emit_fp_load(const minsn& instruction, fp_width width) {
				if (!instruction.arg[0].is_mem())
					throw compile_error{};
				const mmem& memory = instruction.arg[0].mem;
				fp_reg      out    = fp_number(instruction.out);
				if (memory.base == mreg(vreg_cpool)) {
					uint64_t value = constant_pool_value(memory);
					if (width == fp_width::d64)
						code.ldr_literal(width, out, literal64(value));
					else
						code.ldr_literal(width, out, literal32(uint32_t(value)));
					return;
				}
				auto source = legalize_address(memory, width == fp_width::d64 ? 8 : 4);
				code.ldr(width, out, source.base, source.displacement);
			}

			void emit_integer_store(const minsn& instruction, memory_size width) {
				if (!instruction.arg[0].is_mem() || !instruction.arg[1].is_reg() || instruction.arg[0].mem.base == mreg(vreg_cpool))
					throw compile_error{};
				auto destination = legalize_address(instruction.arg[0].mem, memory_bytes(width));
				code.str(width, gp_number(instruction.arg[1].reg), destination.base, destination.displacement);
			}

			void emit_fp_store(const minsn& instruction, fp_width width) {
				if (!instruction.arg[0].is_mem() || !instruction.arg[1].is_reg() || instruction.arg[0].mem.base == mreg(vreg_cpool))
					throw compile_error{};
				auto destination = legalize_address(instruction.arg[0].mem, width == fp_width::d64 ? 8 : 4);
				code.str(width, fp_number(instruction.arg[1].reg), destination.base, destination.displacement);
			}

			void emit_move_integer(const minsn& instruction) {
				gp_reg   out   = gp_number(instruction.out);
				gp_width width = integer_width(instruction.width);
				if (instruction.arg[0].is_const()) {
					uint64_t value = uint64_t(instruction.arg[0].i64);
					code.mov_constant(width, out, width == gp_width::w32 ? uint32_t(value) : value);
				} else if (instruction.arg[0].is_reg() && instruction.arg[0].reg.is_gp()) {
					gp_reg source = gp_number(instruction.arg[0].reg);
					if (source != out)
						code.mov(width, out, source);
				} else if (instruction.arg[0].is_reg() && instruction.arg[0].reg.is_fp()) {
					code.fmov_to_gp(width, out, width == gp_width::w32 ? fp_width::s32 : fp_width::d64, fp_number(instruction.arg[0].reg));
				} else {
					throw compile_error{};
				}
			}

			void emit_move_float(const minsn& instruction) {
				fp_reg   out   = fp_number(instruction.out);
				fp_width width = floating_width(instruction.width);
				if (instruction.arg[0].is_const()) {
					uint64_t value = uint64_t(instruction.arg[0].i64);
					if (width == fp_width::d64)
						code.ldr_literal(width, out, literal64(value));
					else
						code.ldr_literal(width, out, literal32(uint32_t(value)));
				} else if (instruction.arg[0].is_reg() && instruction.arg[0].reg.is_fp()) {
					fp_reg source = fp_number(instruction.arg[0].reg);
					if (source != out)
						code.fmov(width, out, source);
				} else if (instruction.arg[0].is_reg() && instruction.arg[0].reg.is_gp()) {
					code.fmov_from_gp(width, out, width == fp_width::d64 ? gp_width::x64 : gp_width::w32, gp_number(instruction.arg[0].reg));
				} else {
					throw compile_error{};
				}
			}

			void emit_binary_integer(const minsn& instruction, vop operation) {
				gp_width width = integer_width(instruction.width);
				gp_reg   out   = gp_number(instruction.out);
				gp_reg   lhs   = materialize(instruction.arg[0], scratch0, width);
				gp_reg   rhs   = materialize(instruction.arg[1], lhs == scratch0 ? scratch1 : scratch0, width);
				switch (operation) {
					case vop::iadd:
						code.add(width, out, lhs, rhs);
						break;
					case vop::isub:
						code.sub(width, out, lhs, rhs);
						break;
					case vop::imul:
						code.mul(width, out, lhs, rhs);
						break;
					case vop::idiv:
						code.sdiv(width, out, lhs, rhs);
						break;
					case vop::iudiv:
						code.udiv(width, out, lhs, rhs);
						break;
					case vop::iand:
						code.bit_and(width, out, lhs, rhs);
						break;
					case vop::ior:
						code.bit_or(width, out, lhs, rhs);
						break;
					case vop::ixor:
						code.bit_xor(width, out, lhs, rhs);
						break;
					default:
						throw compile_error{};
				}
			}

			void emit_remainder(const minsn& instruction) {
				gp_width width = integer_width(instruction.width);
				gp_reg   out   = gp_number(instruction.out);
				gp_reg   lhs;
				gp_reg   rhs;
				if (instruction.arg[0].is_const()) {
					lhs = materialize(instruction.arg[0], scratch0, width);
				} else {
					lhs = gp_number(instruction.arg[0].reg);
					if (lhs == out) {
						code.mov(width, scratch0, lhs);
						lhs = scratch0;
					}
				}
				if (instruction.arg[1].is_const()) {
					rhs = materialize(instruction.arg[1], lhs == scratch0 ? scratch1 : scratch0, width);
				} else {
					rhs = gp_number(instruction.arg[1].reg);
					if (rhs == out) {
						gp_reg temporary = lhs == scratch0 ? scratch1 : scratch0;
						code.mov(width, temporary, rhs);
						rhs = temporary;
					}
				}
				code.sdiv(width, out, lhs, rhs);
				code.msub(width, out, out, rhs, lhs);
			}

			void emit_shift(const minsn& instruction, vop operation) {
				gp_width width = integer_width(instruction.width);
				gp_reg   out   = gp_number(instruction.out);
				gp_reg   value = materialize(instruction.arg[0], scratch0, width);
				if (instruction.arg[1].is_const()) {
					uint32_t bits   = width == gp_width::x64 ? 64 : 32;
					uint32_t amount = uint32_t(instruction.arg[1].i64) & (bits - 1);
					if (operation == vop::ishl)
						code.lsl_imm(width, out, value, amount);
					else if (operation == vop::ishr)
						code.lsr_imm(width, out, value, amount);
					else
						code.asr_imm(width, out, value, amount);
				} else {
					gp_reg amount = gp_number(instruction.arg[1].reg);
					if (operation == vop::ishl)
						code.lsl_reg(width, out, value, amount);
					else if (operation == vop::ishr)
						code.lsr_reg(width, out, value, amount);
					else
						code.asr_reg(width, out, value, amount);
				}
			}

			void emit_compare_integer(const minsn& instruction) {
				gp_width width = integer_width(instruction.width);
				gp_reg   lhs   = materialize(instruction.arg[0], scratch0, width);
				gp_reg   rhs   = materialize(instruction.arg[1], lhs == scratch0 ? scratch1 : scratch0, width);
				code.cmp(width, lhs, rhs);
				code.cset(gp_width::w32, gp_number(instruction.out), integer_condition(cond(instruction.arg[2].i64)));
			}

			void emit_compare_float(const minsn& instruction) {
				if (!instruction.arg[0].is_reg() || !instruction.arg[1].is_reg())
					throw compile_error{};
				code.fcmp(floating_width(instruction.width), fp_number(instruction.arg[0].reg), fp_number(instruction.arg[1].reg));
				code.cset(gp_width::w32, gp_number(instruction.out), floating_condition(cond(instruction.arg[2].i64)));
			}
			void emit_float_to_integer(const minsn& instruction) {
				if (!instruction.arg[0].is_reg())
					throw compile_error{};
				gp_width width  = integer_width(instruction.width);
				gp_reg   out    = gp_number(instruction.out);
				fp_reg   source = fp_number(instruction.arg[0].reg);
				code.fcvtzs(width, fp_width::d64, out, source);

				// CVTTSD2SI, which defines the established MIR behavior, returns
				// the signed minimum sentinel for NaN and either overflow direction.
				// FCVTZS saturates instead, so detect its invalid domain explicitly.
				double upper = width == gp_width::x64 ? 0x1p63 : 0x1p31;
				double lower = width == gp_width::x64 ? -0x1p63 : -0x1p31;
				code.ldr_literal(fp_width::d64, fscratch, literal64(li::bit_cast<uint64_t>(upper)));
				code.fcmp(fp_width::d64, source, fscratch);
				code.cset(gp_width::w32, scratch0, condition::ge);
				code.cset(gp_width::w32, scratch1, condition::vs);
				code.bit_or(gp_width::w32, scratch0, scratch0, scratch1);

				code.ldr_literal(fp_width::d64, fscratch, literal64(li::bit_cast<uint64_t>(lower)));
				code.fcmp(fp_width::d64, source, fscratch);
				code.cset(gp_width::w32, scratch1, condition::lt);
				code.bit_or(gp_width::w32, scratch0, scratch0, scratch1);

				uint64_t sentinel = width == gp_width::x64 ? uint64_t{1} << 63 : uint64_t{1} << 31;
				code.mov_constant(width, scratch1, sentinel);
				code.cmp_imm(gp_width::w32, scratch0, 0);
				code.csel(width, out, scratch1, out, condition::ne);
				if (instruction.width == mwidth::i8)
					code.sxtb(gp_width::x64, out, out);
				else if (instruction.width == mwidth::i16)
					code.sxth(gp_width::x64, out, out);
				else if (instruction.width == mwidth::i32)
					code.sxtw(out, out);
			}

			void emit_binary_float(const minsn& instruction, vop operation) {
				if (!instruction.arg[0].is_reg() || !instruction.arg[1].is_reg())
					throw compile_error{};
				fp_width width = floating_width(instruction.width);
				fp_reg   out   = fp_number(instruction.out);
				fp_reg   lhs   = fp_number(instruction.arg[0].reg);
				fp_reg   rhs   = fp_number(instruction.arg[1].reg);
				switch (operation) {
					case vop::fadd:
						code.fadd(width, out, lhs, rhs);
						break;
					case vop::fsub:
						code.fsub(width, out, lhs, rhs);
						break;
					case vop::fmul:
						code.fmul(width, out, lhs, rhs);
						break;
					case vop::fdiv:
						code.fdiv(width, out, lhs, rhs);
						break;
					case vop::fmin:
						code.fminnm(width, out, lhs, rhs);
						break;
					case vop::fmax:
						code.fmaxnm(width, out, lhs, rhs);
						break;
					default:
						throw compile_error{};
				}
			}

			void emit_copy_sign(const minsn& instruction) {
				if (!instruction.arg[0].is_reg() || !instruction.arg[1].is_reg())
					throw compile_error{};
				fp_width fw = floating_width(instruction.width);
				gp_width gw = fw == fp_width::d64 ? gp_width::x64 : gp_width::w32;
				code.fmov_to_gp(gw, scratch0, fw, fp_number(instruction.arg[0].reg));
				code.fmov_to_gp(gw, scratch1, fw, fp_number(instruction.arg[1].reg));
				uint64_t magnitude = fw == fp_width::d64 ? 0x7fffffffffffffffull : 0x7fffffffu;
				uint64_t sign      = fw == fp_width::d64 ? 0x8000000000000000ull : 0x80000000u;
				code.and_imm(gw, scratch0, scratch0, magnitude);
				code.and_imm(gw, scratch1, scratch1, sign);
				code.bit_or(gw, scratch0, scratch0, scratch1);
				code.fmov_from_gp(fw, fp_number(instruction.out), gw, scratch0);
			}

			void emit_lea(const minsn& instruction) {
				if (!instruction.arg[0].is_mem() || instruction.arg[0].mem.base == mreg(vreg_cpool))
					throw compile_error{};
				const mmem& memory = instruction.arg[0].mem;
				gp_reg      out    = gp_number(instruction.out);
				gp_reg      base   = gp_number(memory.base);
				if (memory.index) {
					if (base == 31) {
						code.add_imm(gp_width::x64, scratch1, 31, 0);
						code.add(gp_width::x64, out, scratch1, gp_number(memory.index), shift::lsl, memory.shift);
					} else {
						code.add(gp_width::x64, out, base, gp_number(memory.index), shift::lsl, memory.shift);
					}
				} else if (out != base) {
					if (base == 31)
						code.add_imm(gp_width::x64, out, 31, 0);
					else
						code.mov(gp_width::x64, out, base);
				}
				if (!memory.disp)
					return;
				uint32_t immediate;
				uint32_t amount;
				if (add_immediate(memory.disp, immediate, amount)) {
					if (memory.disp > 0)
						code.add_imm(gp_width::x64, out, out, immediate, amount);
					else
						code.sub_imm(gp_width::x64, out, out, immediate, amount);
				} else {
					code.mov_constant(gp_width::x64, scratch0, uint64_t(int64_t(memory.disp)));
					code.add(gp_width::x64, out, out, scratch0);
				}
			}

			void emit_crc32_fallback(gp_reg out) {
				for (unsigned byte = 0; byte != 8; ++byte) {
					code.and_imm(gp_width::w32, scratch1, scratch0, 0xff);
					code.bit_xor(gp_width::w32, out, out, scratch1);
					code.mov_constant(gp_width::w32, scratch1, 0x82f63b78u);
					for (unsigned bit = 0; bit != 8; ++bit) {
						auto even = code.make_label();
						code.tst_imm(gp_width::w32, out, 1);
						code.lsr_imm(gp_width::w32, out, out, 1);
						code.b(condition::eq, even);
						code.bit_xor(gp_width::w32, out, out, scratch1);
						code.bind(even);
					}
					code.lsr_imm(gp_width::x64, scratch0, scratch0, 8);
				}
			}

			void emit_crc32(const minsn& instruction) {
				gp_reg out     = gp_number(instruction.out);
				gp_reg initial = materialize(instruction.arg[0], scratch0, gp_width::w32);
				gp_reg input   = materialize(instruction.arg[1], initial == scratch0 ? scratch1 : scratch0, gp_width::x64);
				if (cpu_has_crc32()) {
					code.crc32c(gp_width::x64, out, initial, input);
				} else {
					// Preserve the data before writing out when allocator coalescing
					// aliases the destination with either input.
					if (instruction.arg[1].is_const())
						materialize(instruction.arg[1], scratch0, gp_width::x64);
					else
						code.mov(gp_width::x64, scratch0, gp_number(instruction.arg[1].reg));
					if (instruction.arg[0].is_const()) {
						materialize(instruction.arg[0], scratch1, gp_width::w32);
						code.mov(gp_width::w32, out, scratch1);
					} else if (out != gp_number(instruction.arg[0].reg)) {
						code.mov(gp_width::w32, out, gp_number(instruction.arg[0].reg));
					}
					emit_crc32_fallback(out);
				}
			}

			void emit_instruction(const minsn& instruction) {
				switch (instruction.op) {
					case vop::null:
						break;
					case vop::movi:
						emit_move_integer(instruction);
						break;
					case vop::movf:
						emit_move_float(instruction);
						break;
					case vop::izx8: {
						gp_reg source = materialize(instruction.arg[0], scratch0, gp_width::w32);
						code.and_imm(gp_width::w32, gp_number(instruction.out), source, 0xff);
						break;
					}
					case vop::izx16: {
						gp_reg source = materialize(instruction.arg[0], scratch0, gp_width::w32);
						code.and_imm(gp_width::w32, gp_number(instruction.out), source, 0xffff);
						break;
					}
					case vop::izx32: {
						gp_reg source = materialize(instruction.arg[0], scratch0, gp_width::w32);
						code.mov(gp_width::w32, gp_number(instruction.out), source);
						break;
					}
					case vop::isx8:
						code.sxtb(gp_width::x64, gp_number(instruction.out), materialize(instruction.arg[0], scratch0, gp_width::x64));
						break;
					case vop::isx16:
						code.sxth(gp_width::x64, gp_number(instruction.out), materialize(instruction.arg[0], scratch0, gp_width::x64));
						break;
					case vop::isx32:
						code.sxtw(gp_number(instruction.out), materialize(instruction.arg[0], scratch0, gp_width::x64));
						break;
					case vop::fx32:
						code.fcvt(fp_width::s32, fp_number(instruction.out), fp_width::d64, fp_number(instruction.arg[0].reg));
						break;
					case vop::fx64:
						code.fcvt(fp_width::d64, fp_number(instruction.out), fp_width::s32, fp_number(instruction.arg[0].reg));
						break;
					case vop::icvt:
						emit_float_to_integer(instruction);
						break;
					case vop::fcvt:
						code.scvtf(fp_width::d64, integer_width(instruction.width), fp_number(instruction.out), gp_number(instruction.arg[0].reg));
						break;
					case vop::loadi8:
						emit_integer_load(instruction, memory_size::u8);
						break;
					case vop::loadi16:
						emit_integer_load(instruction, memory_size::u16);
						break;
					case vop::loadi32:
						emit_integer_load(instruction, memory_size::u32);
						break;
					case vop::loadi64:
						emit_integer_load(instruction, memory_size::u64);
						break;
					case vop::loadf32:
						emit_fp_load(instruction, fp_width::s32);
						break;
					case vop::loadf64:
						emit_fp_load(instruction, fp_width::d64);
						break;
					case vop::storei8:
						emit_integer_store(instruction, memory_size::u8);
						break;
					case vop::storei16:
						emit_integer_store(instruction, memory_size::u16);
						break;
					case vop::storei32:
						emit_integer_store(instruction, memory_size::u32);
						break;
					case vop::storei64:
						emit_integer_store(instruction, memory_size::u64);
						break;
					case vop::storef32:
						emit_fp_store(instruction, fp_width::s32);
						break;
					case vop::storef64:
						emit_fp_store(instruction, fp_width::d64);
						break;
					case vop::iadd:
					case vop::isub:
					case vop::imul:
					case vop::idiv:
					case vop::iudiv:
					case vop::iand:
					case vop::ior:
					case vop::ixor:
						emit_binary_integer(instruction, instruction.getv());
						break;
					case vop::imod:
						emit_remainder(instruction);
						break;
					case vop::ineg:
						code.neg(integer_width(instruction.width), gp_number(instruction.out),
							 materialize(instruction.arg[0], scratch0, integer_width(instruction.width)));
						break;
					case vop::inot:
						code.bit_not(integer_width(instruction.width), gp_number(instruction.out),
							 materialize(instruction.arg[0], scratch0, integer_width(instruction.width)));
						break;
					case vop::ishl:
					case vop::ishr:
					case vop::isar:
						emit_shift(instruction, instruction.getv());
						break;
					case vop::fadd:
					case vop::fsub:
					case vop::fmul:
					case vop::fdiv:
					case vop::fmin:
					case vop::fmax:
						emit_binary_float(instruction, instruction.getv());
						break;
					case vop::fneg:
						code.fneg(floating_width(instruction.width), fp_number(instruction.out), fp_number(instruction.arg[0].reg));
						break;
					case vop::fabs:
						code.fabs(floating_width(instruction.width), fp_number(instruction.out), fp_number(instruction.arg[0].reg));
						break;
					case vop::fsqrt:
						code.fsqrt(floating_width(instruction.width), fp_number(instruction.out), fp_number(instruction.arg[0].reg));
						break;
					case vop::fround: {
						static constexpr std::array<fp_round, 4> modes = {
							 fp_round::nearest_even,
							 fp_round::minus_infinity,
							 fp_round::plus_infinity,
							 fp_round::toward_zero,
						};
						size_t mode = size_t(instruction.arg[1].i64);
						if (mode >= modes.size())
							throw compile_error{};
						code.frint(floating_width(instruction.width), fp_number(instruction.out), fp_number(instruction.arg[0].reg), modes[mode]);
						break;
					}
					case vop::fcopysign:
						emit_copy_sign(instruction);
						break;
					case vop::icmp:
						emit_compare_integer(instruction);
						break;
					case vop::fcmp:
						emit_compare_float(instruction);
						break;
					case vop::lea:
						emit_lea(instruction);
						break;
					case vop::select: {
						gp_reg boolean = gp_number(instruction.arg[0].reg);
						code.cmp_imm(gp_width::w32, boolean, 0);
						if (instruction.out.is_gp())
							code.csel(integer_width(instruction.width), gp_number(instruction.out), gp_number(instruction.arg[1].reg),
								 gp_number(instruction.arg[2].reg), condition::ne);
						else
							code.fcsel(floating_width(instruction.width), fp_number(instruction.out), fp_number(instruction.arg[1].reg),
								 fp_number(instruction.arg[2].reg), condition::ne);
						break;
					}
					case vop::crc32:
						emit_crc32(instruction);
						break;
					case vop::rdcycle:
						code.rdcycle(gp_number(instruction.out));
						break;
					case vop::call:
						if (instruction.arg[0].is_const())
							code.bl_absolute(uint64_t(instruction.arg[0].i64));
						else if (instruction.arg[0].is_reg())
							code.blr(gp_number(instruction.arg[0].reg));
						else
							throw compile_error{};
						break;
					case vop::js: {
						size_t true_id  = size_t(instruction.arg[1].i64);
						size_t false_id = size_t(instruction.arg[2].i64);
						if (true_id >= block_labels.size() || false_id >= block_labels.size())
							throw compile_error{};
						code.cbnz(gp_width::w32, gp_number(instruction.arg[0].reg), block_labels[true_id]);
						code.b(block_labels[false_id]);
						break;
					}
					case vop::jmp: {
						size_t target = size_t(instruction.arg[0].i64);
						if (target >= block_labels.size())
							throw compile_error{};
						code.b(block_labels[target]);
						break;
					}
					case vop::ret:
						if (instruction.arg[0].is_reg() && gp_number(instruction.arg[0].reg) != 0)
							code.mov(gp_width::x64, 0, gp_number(instruction.arg[0].reg));
						epilogue();
						break;
					case vop::unreachable:
						code.brk();
						break;
				}
			}

		  public:
			explicit assembler(mprocedure& procedure) : proc(procedure) {}

			[[nodiscard]] size_t native_stack_bytes() const noexcept {
				// STP saves the caller's frame pointer and link register before the
				// aligned area containing outgoing arguments, spills, and callee saves.
				return 2 * sizeof(uint64_t) + frame_size;
			}

			std::span<const uint8_t> assemble(uintptr_t load_address) {
				if (block_labels.empty()) {
					proc.basic_blocks.sort([](const mblock& lhs, const mblock& rhs) { return lhs.uid < rhs.uid; });
					block_labels.resize(proc.next_block);
					for (auto& label : block_labels)
						label = code.make_label();
					prologue();
					for (const mblock& block : proc.basic_blocks) {
						if (block.uid >= block_labels.size())
							throw compile_error{};
						code.bind(block_labels[block.uid]);
						for (const minsn& instruction : block.instructions)
							emit_instruction(instruction);
					}
				}
				return code.bytes(load_address);
			}
		};

		std::string register_name(bool fp, unsigned number, bool wide = true) {
			if (fp)
				return std::string(wide ? "d" : "s") + std::to_string(number);
			if (number == 31)
				return wide ? "xzr/sp" : "wzr/wsp";
			return std::string(wide ? "x" : "w") + std::to_string(number);
		}

		std::string decode_word(uint32_t word, uintptr_t pc) {
			std::ostringstream text;
			auto               rd = unsigned(word & 31);
			auto               rn = unsigned((word >> 5) & 31);
			auto               rm = unsigned((word >> 16) & 31);
			if (word == 0xd503201f)
				return "nop";
			if ((word & 0xffe0001f) == 0xd4200000)
				return "brk #" + std::to_string((word >> 5) & 0xffff);
			if ((word & 0xfffffc1f) == 0xd65f0000)
				return "ret " + register_name(false, rn);
			if ((word & 0xfffffc1f) == 0xd61f0000)
				return "br " + register_name(false, rn);
			if ((word & 0xfffffc1f) == 0xd63f0000)
				return "blr " + register_name(false, rn);
			if ((word & 0xffffffe0) == 0xd53be040)
				return "mrs " + register_name(false, rd) + ", cntvct_el0";
			if ((word & 0x7c000000) == 0x14000000) {
				int64_t delta = int64_t(int32_t(word << 6)) >> 4;
				text << ((word >> 31) ? "bl " : "b ") << "0x" << std::hex << (pc + delta);
				return text.str();
			}
			if ((word & 0xff000010) == 0x54000000) {
				static constexpr std::array<const char*, 16> names = {
					 "eq",
					 "ne",
					 "hs",
					 "lo",
					 "mi",
					 "pl",
					 "vs",
					 "vc",
					 "hi",
					 "ls",
					 "ge",
					 "lt",
					 "gt",
					 "le",
					 "al",
					 "nv",
				};
				int64_t delta = int64_t(int32_t(word << 8)) >> 11;
				text << "b." << names[word & 15] << " 0x" << std::hex << (pc + delta);
				return text.str();
			}
			if ((word & 0x7e000000) == 0x34000000) {
				int64_t delta = int64_t(int32_t(word << 8)) >> 11;
				text << ((word & 0x01000000) ? "cbnz " : "cbz ") << register_name(false, rd, (word >> 31) != 0) << ", 0x" << std::hex << (pc + delta);
				return text.str();
			}
			if ((word & 0x9f000000) == 0x90000000) {
				int64_t immediate = int64_t(((uint64_t(word >> 5) & 0x7ffff) << 2) | (word >> 29 & 3));
				if (immediate & (int64_t{1} << 20))
					immediate |= ~((int64_t{1} << 21) - 1);
				int64_t byte_delta = immediate * 4096;
				text << "adrp " << register_name(false, rd) << ", 0x" << std::hex << ((pc & ~uintptr_t{0xfff}) + byte_delta);
				return text.str();
			}
			if ((word & 0x3b000000) == 0x18000000) {
				bool    fp    = (word & 0x04000000) != 0;
				bool    wide  = (word & 0x40000000) != 0;
				int64_t delta = int64_t(int32_t(word << 8)) >> 11;
				text << "ldr " << register_name(fp, rd, wide) << ", 0x" << std::hex << (pc + delta);
				return text.str();
			}
			if ((word & 0x3a000000) == 0x28000000) {
				bool     fp     = (word & 0x04000000) != 0;
				bool     load   = (word & 0x00400000) != 0;
				bool     wide   = fp ? (word & 0x40000000) != 0 : (word >> 31) != 0;
				unsigned second = (word >> 10) & 31;
				text << (load ? "ldp " : "stp ") << register_name(fp, rd, wide) << ", " << register_name(fp, second, wide) << ", [" << register_name(false, rn)
					  << "]";
				return text.str();
			}
			if ((word & 0x7f800000) == 0x52800000 || (word & 0x7f800000) == 0x72800000) {
				bool wide = (word >> 31) != 0;
				text << (((word & 0x7f800000) == 0x52800000) ? "movz " : "movk ") << register_name(false, rd, wide) << ", #0x" << std::hex << ((word >> 5) & 0xffff)
					  << ", lsl #" << std::dec << (((word >> 21) & 3) * 16);
				return text.str();
			}
			if ((word & 0x1f000000) == 0x11000000) {
				bool sub   = (word & 0x40000000) != 0;
				bool flags = (word & 0x20000000) != 0;
				bool wide  = (word >> 31) != 0;
				text << (sub ? (flags ? "subs " : "sub ") : (flags ? "adds " : "add ")) << register_name(false, rd, wide) << ", " << register_name(false, rn, wide)
					  << ", #" << ((word >> 10) & 0xfff);
				if (word & (1u << 22))
					text << ", lsl #12";
				return text.str();
			}
			if ((word & 0x1f200000) == 0x0b000000) {
				bool sub   = (word & 0x40000000) != 0;
				bool flags = (word & 0x20000000) != 0;
				bool wide  = (word >> 31) != 0;
				text << (sub ? (flags ? "subs " : "sub ") : (flags ? "adds " : "add ")) << register_name(false, rd, wide) << ", " << register_name(false, rn, wide)
					  << ", " << register_name(false, rm, wide);
				return text.str();
			}
			if ((word & 0x1f000000) == 0x0a000000) {
				static constexpr std::array<const char*, 4> plain    = {"and", "orr", "eor", "ands"};
				static constexpr std::array<const char*, 4> inverted = {"bic", "orn", "eon", "bics"};
				bool                                        wide     = (word >> 31) != 0;
				auto                                        name     = (word & 0x00200000) ? inverted[(word >> 29) & 3] : plain[(word >> 29) & 3];
				text << name << " " << register_name(false, rd, wide) << ", " << register_name(false, rn, wide) << ", " << register_name(false, rm, wide);
				return text.str();
			}
			if ((word & 0x1f800000) == 0x12000000) {
				static constexpr std::array<const char*, 4> names = {"and", "orr", "eor", "ands"};
				bool                                        wide  = (word >> 31) != 0;
				text << names[(word >> 29) & 3] << " " << register_name(false, rd, wide) << ", " << register_name(false, rn, wide) << ", #<bitmask>";
				return text.str();
			}
			if ((word & 0x7fe0fc00) == 0x1b007c00) {
				bool wide = (word >> 31) != 0;
				text << "mul " << register_name(false, rd, wide) << ", " << register_name(false, rn, wide) << ", " << register_name(false, rm, wide);
				return text.str();
			}
			if ((word & 0x7fe08000) == 0x1b008000) {
				bool     wide = (word >> 31) != 0;
				unsigned ra   = (word >> 10) & 31;
				text << "msub " << register_name(false, rd, wide) << ", " << register_name(false, rn, wide) << ", " << register_name(false, rm, wide) << ", "
					  << register_name(false, ra, wide);
				return text.str();
			}
			if ((word & 0x7fe00c00) == 0x1a800000 || (word & 0x7fe00c00) == 0x1a800400) {
				bool wide = (word >> 31) != 0;
				text << (((word & 0x400) != 0) ? "cset " : "csel ") << register_name(false, rd, wide);
				if ((word & 0x400) == 0)
					text << ", " << register_name(false, rn, wide) << ", " << register_name(false, rm, wide);
				return text.str();
			}
			if ((word & 0x7fe0fc00) == 0x1ac00c00 || (word & 0x7fe0fc00) == 0x1ac00800) {
				text << (((word & 0x7fe0fc00) == 0x1ac00c00) ? "sdiv " : "udiv ") << register_name(false, rd, (word >> 31) != 0) << ", "
					  << register_name(false, rn, (word >> 31) != 0) << ", " << register_name(false, rm, (word >> 31) != 0);
				return text.str();
			}
			if ((word & 0x7fe0fc00) == 0x1ac05800 || (word & 0x7fe0fc00) == 0x1ac05c00) {
				text << "crc32c " << register_name(false, rd, false) << ", " << register_name(false, rn, false) << ", "
					  << register_name(false, rm, (word >> 31) != 0);
				return text.str();
			}
			if ((word & 0x1f000000) == 0x1e000000) {
				bool wide = (word & 0x00400000) != 0;
				if ((word & 0xffa0fc1f) == 0x1e202000) {
					text << "fcmp " << register_name(true, rn, wide) << ", " << register_name(true, rm, wide);
					return text.str();
				}
				if ((word & 0xffa00c00) == 0x1e200c00) {
					text << "fcsel " << register_name(true, rd, wide) << ", " << register_name(true, rn, wide) << ", " << register_name(true, rm, wide);
					return text.str();
				}
				struct fp_operation {
					uint32_t    base;
					const char* name;
				};
				static constexpr std::array binary_fp_ops = {
					 fp_operation{0x1e202800, "fadd"},
					 fp_operation{0x1e203800, "fsub"},
					 fp_operation{0x1e200800, "fmul"},
					 fp_operation{0x1e201800, "fdiv"},
					 fp_operation{0x1e204800, "fmax"},
					 fp_operation{0x1e205800, "fmin"},
					 fp_operation{0x1e206800, "fmaxnm"},
					 fp_operation{0x1e207800, "fminnm"},
				};
				for (auto operation : binary_fp_ops) {
					if ((word & 0xffa0fc00) == operation.base) {
						text << operation.name << " " << register_name(true, rd, wide) << ", " << register_name(true, rn, wide) << ", "
							  << register_name(true, rm, wide);
						return text.str();
					}
				}
				static constexpr std::array unary_fp_ops = {
					 fp_operation{0x1e204000, "fmov"},
					 fp_operation{0x1e20c000, "fabs"},
					 fp_operation{0x1e214000, "fneg"},
					 fp_operation{0x1e21c000, "fsqrt"},
					 fp_operation{0x1e244000, "frintn"},
					 fp_operation{0x1e24c000, "frintp"},
					 fp_operation{0x1e254000, "frintm"},
					 fp_operation{0x1e25c000, "frintz"},
					 fp_operation{0x1e264000, "frinta"},
					 fp_operation{0x1e274000, "frintx"},
					 fp_operation{0x1e27c000, "frinti"},
				};
				for (auto operation : unary_fp_ops) {
					if ((word & 0xffbffc00) == operation.base) {
						text << operation.name << " " << register_name(true, rd, wide) << ", " << register_name(true, rn, wide);
						return text.str();
					}
				}
			}
			if ((word & 0x3b000000) == 0x39000000 || (word & 0x3b000000) == 0x38000000 || (word & 0x3f000000) == 0x3d000000 || (word & 0x3f000000) == 0x3c000000) {
				bool fp   = (word & 0x04000000) != 0;
				bool load = (word & 0x00400000) != 0;
				bool wide = fp ? (word & 0x40000000) != 0 : ((word >> 30) & 3) == 3;
				text << (load ? "ldr " : "str ") << register_name(fp, rd, wide) << ", [" << register_name(false, rn) << "]";
				return text.str();
			}
			text << ".word 0x" << std::hex << std::setw(8) << std::setfill('0') << word;
			return text.str();
		}
	}

	jfunction* assemble_ir(mprocedure* procedure) {
		if (!procedure)
			return nullptr;
		procedure->assembly_error.clear();
		if (!procedure->source || !procedure->source->L || !procedure->source->f) {
			if (procedure->source)
				procedure->source->inline_caches.reset();
			procedure->assembly_error = "A64 assembly requires a procedure with VM and function owners";
			return nullptr;
		}
		try {
			assembler output(*procedure);
			auto      preliminary = output.assemble(0);
			if (preliminary.empty())
				throw compile_error{"A64 assembler produced no code"};

			std::error_code error;
			auto            memory = platform::code_memory::allocate(preliminary.size(), error);
			if (error)
				throw compile_error{code_memory_failure("allocation", error)};
			auto final = output.assemble(reinterpret_cast<uintptr_t>(memory.data()));
			if (final.size() != preliminary.size())
				throw compile_error{"A64 relocation changed code size from " + std::to_string(preliminary.size()) + " to " + std::to_string(final.size())};
			if (error = memory.write(0, std::as_bytes(final)); error)
				throw compile_error{code_memory_failure("write", error)};
			if (error = memory.publish(); error)
				throw compile_error{code_memory_failure("publication", error)};

			jfunction* result = jfunction::create(procedure->source->L, procedure->source->f, std::move(memory),
				 jfunction::layout{
					  .code_size          = final.size(),
					  .native_stack_bytes = output.native_stack_bytes(),
					  .vm_stack_slots     = size_t(procedure->source->f->num_locals) + size_t(procedure->source->max_stack_slot),
					  .osr_target         = procedure->source->osr_target,
				 },
				 procedure->const_pool, std::move(procedure->source->inline_caches));
	#if LI_VTUNE
			iJIT_Method_Load load{};
			load.method_id           = iJIT_GetNewMethodID();
			load.method_load_address = const_cast<void*>(result->entry_address());
			load.method_size         = uint32_t(final.size());
			load.method_name         = const_cast<char*>("Lightning A64 JIT Code");
			result->uid              = load.method_id;
			iJIT_NotifyEvent(iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED, &load);
	#endif
			return result;
		} catch (const std::exception& error) {
			procedure->source->inline_caches.reset();
			procedure->assembly_error = error.what();
			return nullptr;
		} catch (...) {
			procedure->source->inline_caches.reset();
			procedure->assembly_error = "A64 assembly failed with a non-standard exception";
			return nullptr;
		}
	}

	std::string disassemble_code(const jfunction& function) {
		std::span<const std::byte> bytes = function.code_bytes();
		std::ostringstream         output;
		uintptr_t                  base = reinterpret_cast<uintptr_t>(function.entry_address());
		for (size_t offset = 0; offset + 4 <= bytes.size(); offset += 4) {
			uint32_t word;
			std::memcpy(&word, bytes.data() + offset, sizeof(word));
			output << std::hex << std::setw(16) << std::setfill('0') << (base + offset) << "  " << std::setw(8) << word << "  " << decode_word(word, base + offset)
					 << '\n';
		}
		if (bytes.size() & 3)
			output << ".byte <truncated A64 instruction>\n";
		return output.str();
	}
}

#endif
