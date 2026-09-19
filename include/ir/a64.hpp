#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace li::ir::a64 {
	/// Integer and floating-point register operands are architectural IDs in the range 0..31.
	/// For GP operands, ID 31 is SP when used as a memory base or ADD/SUB-immediate source.
	/// It is also SP as a non-flag-setting ADD/SUB-immediate destination; flag-setting
	/// destinations and every other integer operand interpret 31 as ZR.
	using gp_reg = uint8_t;
	using fp_reg = uint8_t;

	enum class gp_width : uint8_t { w32 = 32, x64 = 64 };
	enum class fp_width : uint8_t { s32 = 32, d64 = 64 };
	enum class memory_size : uint8_t { u8 = 0, u16 = 1, u32 = 2, u64 = 3 };
	enum class shift : uint8_t { lsl = 0, lsr = 1, asr = 2, ror = 3 };
	enum class index_extend : uint8_t { uxtw = 2, lsl = 3, sxtw = 6, sxtx = 7 };
	enum class pair_mode : uint8_t { offset, pre_index, post_index };
	enum class condition : uint8_t {
		eq = 0,
		ne = 1,
		cs = 2,
		hs = cs,
		cc = 3,
		lo = cc,
		mi = 4,
		pl = 5,
		vs = 6,
		vc = 7,
		hi = 8,
		ls = 9,
		ge = 10,
		lt = 11,
		gt = 12,
		le = 13,
		al = 14,
		nv = 15,
	};
	enum class fp_round : uint8_t {
		nearest_even,
		plus_infinity,
		minus_infinity,
		toward_zero,
		nearest_away,
		current_exact,
		current,
	};

	/// Reverses a condition code; instruction emitters reject the non-predicates AL and NV.
	inline constexpr condition invert(condition value) { return static_cast<condition>(static_cast<uint8_t>(value) ^ 1u); }

	/// A hand encoder for fixed-width little-endian A64 instructions.
	///
	/// Label branches start compact and are relaxed by finish(). Out-of-range conditional
	/// branches grow to an inverted condition plus a branch. Branches outside imm26 reach use
	/// an X16 ADRP/ADD/BR veneer; calls use BLR so their return address is the end of the veneer.
	/// X16 must therefore remain unavailable to the register allocator.
	class emitter {
	  public:
		struct label {
			uint32_t    id    = std::numeric_limits<uint32_t>::max();
			const void* owner = nullptr;
		};
		struct literal {
			uint32_t    id    = std::numeric_limits<uint32_t>::max();
			const void* owner = nullptr;
		};

		enum class relocation_kind : uint8_t {
			branch26,
			conditional19,
			compare_branch19,
			literal19,
			adr_page21,
			add_pageoff12,
			load_pageoff12,
			movw_uabs_g0,
			movw_uabs_g1,
			movw_uabs_g2,
			movw_uabs_g3,
			absolute64,
		};
		enum class relocation_target_kind : uint8_t { internal_label, literal_pool, absolute_address };
		struct relocation_target {
			relocation_target_kind kind    = relocation_target_kind::absolute_address;
			uint32_t               id      = 0;
			uint64_t               address = 0;
		};
		struct relocation {
			relocation_kind   kind;
			uint64_t          byte_offset;
			relocation_target target;
			int64_t           addend;
		};

		emitter()                          = default;
		emitter(const emitter&)            = delete;
		emitter& operator=(const emitter&) = delete;
		emitter(emitter&&)                 = delete;
		emitter& operator=(emitter&&)      = delete;

		/// Creates an unbound label owned by this emitter.
		label make_label() {
			labels_.push_back(false);
			return {static_cast<uint32_t>(labels_.size() - 1), this};
		}

		/// Binds a label at the current instruction-stream position.
		void bind(label target) {
			check_label(target);
			if (labels_[target.id])
				throw std::logic_error("A64 label is already bound");
			labels_[target.id] = true;
			item value;
			value.kind   = item_kind::label;
			value.target = target;
			append_item(value);
		}

		/// Emits one already encoded instruction word. Prefer the checked instruction helpers.
		void emit_word(uint32_t word) {
			item value;
			value.kind = item_kind::word;
			value.word = word;
			append_item(value);
		}

		/// Emits count architectural NOPs without constructing one layout node per instruction.
		void nops(size_t count) {
			if (!count)
				return;
			item value;
			value.kind  = item_kind::padding;
			value.count = count;
			append_item(value);
		}

		void nop() { emit_word(0xd503201f); }

		void add(gp_width width, gp_reg rd, gp_reg rn, gp_reg rm, shift kind = shift::lsl, uint32_t amount = 0) {
			emit_word(add_sub_reg(false, false, width, rd, rn, rm, kind, amount));
		}
		void adds(gp_width width, gp_reg rd, gp_reg rn, gp_reg rm, shift kind = shift::lsl, uint32_t amount = 0) {
			emit_word(add_sub_reg(false, true, width, rd, rn, rm, kind, amount));
		}
		void sub(gp_width width, gp_reg rd, gp_reg rn, gp_reg rm, shift kind = shift::lsl, uint32_t amount = 0) {
			emit_word(add_sub_reg(true, false, width, rd, rn, rm, kind, amount));
		}
		void subs(gp_width width, gp_reg rd, gp_reg rn, gp_reg rm, shift kind = shift::lsl, uint32_t amount = 0) {
			emit_word(add_sub_reg(true, true, width, rd, rn, rm, kind, amount));
		}
		void add_imm(gp_width width, gp_reg rd, gp_reg rn, uint32_t immediate, uint32_t shift_amount = 0) {
			emit_word(add_sub_imm(false, false, width, rd, rn, immediate, shift_amount));
		}
		void adds_imm(gp_width width, gp_reg rd, gp_reg rn, uint32_t immediate, uint32_t shift_amount = 0) {
			emit_word(add_sub_imm(false, true, width, rd, rn, immediate, shift_amount));
		}
		void sub_imm(gp_width width, gp_reg rd, gp_reg rn, uint32_t immediate, uint32_t shift_amount = 0) {
			emit_word(add_sub_imm(true, false, width, rd, rn, immediate, shift_amount));
		}
		void subs_imm(gp_width width, gp_reg rd, gp_reg rn, uint32_t immediate, uint32_t shift_amount = 0) {
			emit_word(add_sub_imm(true, true, width, rd, rn, immediate, shift_amount));
		}
		void mul(gp_width width, gp_reg rd, gp_reg rn, gp_reg rm) {
			check_regs(rd, rn, rm);
			emit_word(width_bit(width) | 0x1b007c00u | (uint32_t(rm) << 16) | (uint32_t(rn) << 5) | rd);
		}
		/// Computes rd = ra - (rn * rm). This is the MSUB form used to lower signed remainder.
		void msub(gp_width width, gp_reg rd, gp_reg rn, gp_reg rm, gp_reg ra) {
			check_regs(rd, rn, rm, ra);
			emit_word(width_bit(width) | 0x1b008000u | (uint32_t(rm) << 16) | (uint32_t(ra) << 10) | (uint32_t(rn) << 5) | rd);
		}
		void sdiv(gp_width width, gp_reg rd, gp_reg rn, gp_reg rm) {
			check_regs(rd, rn, rm);
			emit_word(width_bit(width) | 0x1ac00c00u | (uint32_t(rm) << 16) | (uint32_t(rn) << 5) | rd);
		}
		void udiv(gp_width width, gp_reg rd, gp_reg rn, gp_reg rm) {
			check_regs(rd, rn, rm);
			emit_word(width_bit(width) | 0x1ac00800u | (uint32_t(rm) << 16) | (uint32_t(rn) << 5) | rd);
		}

		void bit_and(gp_width width, gp_reg rd, gp_reg rn, gp_reg rm, shift kind = shift::lsl, uint32_t amount = 0) {
			emit_word(logical_reg(0, width, rd, rn, rm, kind, amount));
		}
		void bit_or(gp_width width, gp_reg rd, gp_reg rn, gp_reg rm, shift kind = shift::lsl, uint32_t amount = 0) {
			emit_word(logical_reg(1, width, rd, rn, rm, kind, amount));
		}
		void bit_xor(gp_width width, gp_reg rd, gp_reg rn, gp_reg rm, shift kind = shift::lsl, uint32_t amount = 0) {
			emit_word(logical_reg(2, width, rd, rn, rm, kind, amount));
		}
		void bit_ands(gp_width width, gp_reg rd, gp_reg rn, gp_reg rm, shift kind = shift::lsl, uint32_t amount = 0) {
			emit_word(logical_reg(3, width, rd, rn, rm, kind, amount));
		}
		void and_imm(gp_width width, gp_reg rd, gp_reg rn, uint64_t immediate) { emit_word(logical_imm(0, width, rd, rn, immediate)); }
		void or_imm(gp_width width, gp_reg rd, gp_reg rn, uint64_t immediate) { emit_word(logical_imm(1, width, rd, rn, immediate)); }
		void xor_imm(gp_width width, gp_reg rd, gp_reg rn, uint64_t immediate) { emit_word(logical_imm(2, width, rd, rn, immediate)); }
		void ands_imm(gp_width width, gp_reg rd, gp_reg rn, uint64_t immediate) { emit_word(logical_imm(3, width, rd, rn, immediate)); }
		void mov(gp_width width, gp_reg rd, gp_reg rn) { bit_or(width, rd, 31, rn); }
		void neg(gp_width width, gp_reg rd, gp_reg rn) { sub(width, rd, 31, rn); }
		void bit_not(gp_width width, gp_reg rd, gp_reg rn, shift kind = shift::lsl, uint32_t amount = 0) {
			check_regs(rd, rn);
			check_shift(width, kind, amount);
			emit_word(width_bit(width) | 0x2a200000u | (uint32_t(kind) << 22) | (uint32_t(rn) << 16) | (uint32_t(amount) << 10) | (31u << 5) | rd);
		}
		void sxtb(gp_width width, gp_reg rd, gp_reg rn) { signed_extend(width, rd, rn, 8); }
		void sxth(gp_width width, gp_reg rd, gp_reg rn) { signed_extend(width, rd, rn, 16); }
		void sxtw(gp_reg rd, gp_reg rn) { signed_extend(gp_width::x64, rd, rn, 32); }

		void lsl_imm(gp_width width, gp_reg rd, gp_reg rn, uint32_t amount) { bitfield_shift(width, rd, rn, amount, shift::lsl); }
		void lsr_imm(gp_width width, gp_reg rd, gp_reg rn, uint32_t amount) { bitfield_shift(width, rd, rn, amount, shift::lsr); }
		void asr_imm(gp_width width, gp_reg rd, gp_reg rn, uint32_t amount) { bitfield_shift(width, rd, rn, amount, shift::asr); }
		void lsl_reg(gp_width width, gp_reg rd, gp_reg rn, gp_reg rm) { variable_shift(width, rd, rn, rm, 0); }
		void lsr_reg(gp_width width, gp_reg rd, gp_reg rn, gp_reg rm) { variable_shift(width, rd, rn, rm, 1); }
		void asr_reg(gp_width width, gp_reg rd, gp_reg rn, gp_reg rm) { variable_shift(width, rd, rn, rm, 2); }
		void ror_reg(gp_width width, gp_reg rd, gp_reg rn, gp_reg rm) { variable_shift(width, rd, rn, rm, 3); }

		void cmp(gp_width width, gp_reg rn, gp_reg rm, shift kind = shift::lsl, uint32_t amount = 0) { subs(width, 31, rn, rm, kind, amount); }
		void cmp_imm(gp_width width, gp_reg rn, uint32_t immediate, uint32_t shift_amount = 0) { subs_imm(width, 31, rn, immediate, shift_amount); }
		void cmn(gp_width width, gp_reg rn, gp_reg rm, shift kind = shift::lsl, uint32_t amount = 0) { adds(width, 31, rn, rm, kind, amount); }
		void tst(gp_width width, gp_reg rn, gp_reg rm, shift kind = shift::lsl, uint32_t amount = 0) { bit_ands(width, 31, rn, rm, kind, amount); }
		void tst_imm(gp_width width, gp_reg rn, uint64_t immediate) { ands_imm(width, 31, rn, immediate); }

		void csel(gp_width width, gp_reg rd, gp_reg rn, gp_reg rm, condition predicate) {
			check_regs(rd, rn, rm);
			check_condition(predicate);
			emit_word(width_bit(width) | 0x1a800000u | (uint32_t(rm) << 16) | (uint32_t(predicate) << 12) | (uint32_t(rn) << 5) | rd);
		}
		void cset(gp_width width, gp_reg rd, condition predicate) {
			check_reg(rd);
			check_condition(predicate);
			condition encoded = invert(predicate);
			emit_word(width_bit(width) | 0x1a800400u | (31u << 16) | (uint32_t(encoded) << 12) | (31u << 5) | rd);
		}
		void crc32c(gp_width input_width, gp_reg rd, gp_reg rn, gp_reg rm) {
			check_regs(rd, rn, rm);
			check_width(input_width);
			uint32_t base = input_width == gp_width::x64 ? 0x9ac05c00u : 0x1ac05800u;
			emit_word(base | (uint32_t(rm) << 16) | (uint32_t(rn) << 5) | rd);
		}
		/// Reads the architected virtual counter. Supported AArch64 user ABIs expose CNTVCT_EL0.
		void rdcycle(gp_reg rt) {
			check_reg(rt);
			emit_word(0xd53be040u | rt);
		}

		void fmov(fp_width width, fp_reg rd, fp_reg rn) { fp_unary(0x1e204000u, width, rd, rn); }
		void fabs(fp_width width, fp_reg rd, fp_reg rn) { fp_unary(0x1e20c000u, width, rd, rn); }
		void fneg(fp_width width, fp_reg rd, fp_reg rn) { fp_unary(0x1e214000u, width, rd, rn); }
		void fsqrt(fp_width width, fp_reg rd, fp_reg rn) { fp_unary(0x1e21c000u, width, rd, rn); }
		void fadd(fp_width width, fp_reg rd, fp_reg rn, fp_reg rm) { fp_binary(0x1e202800u, width, rd, rn, rm); }
		void fsub(fp_width width, fp_reg rd, fp_reg rn, fp_reg rm) { fp_binary(0x1e203800u, width, rd, rn, rm); }
		void fmul(fp_width width, fp_reg rd, fp_reg rn, fp_reg rm) { fp_binary(0x1e200800u, width, rd, rn, rm); }
		void fdiv(fp_width width, fp_reg rd, fp_reg rn, fp_reg rm) { fp_binary(0x1e201800u, width, rd, rn, rm); }
		void fmax(fp_width width, fp_reg rd, fp_reg rn, fp_reg rm) { fp_binary(0x1e204800u, width, rd, rn, rm); }
		void fmin(fp_width width, fp_reg rd, fp_reg rn, fp_reg rm) { fp_binary(0x1e205800u, width, rd, rn, rm); }
		void fmaxnm(fp_width width, fp_reg rd, fp_reg rn, fp_reg rm) { fp_binary(0x1e206800u, width, rd, rn, rm); }
		void fminnm(fp_width width, fp_reg rd, fp_reg rn, fp_reg rm) { fp_binary(0x1e207800u, width, rd, rn, rm); }
		void frint(fp_width width, fp_reg rd, fp_reg rn, fp_round mode) {
			static constexpr std::array<uint32_t, 7> bases = {
				 0x1e244000u,
				 0x1e24c000u,
				 0x1e254000u,
				 0x1e25c000u,
				 0x1e264000u,
				 0x1e274000u,
				 0x1e27c000u,
			};
			uint8_t index = static_cast<uint8_t>(mode);
			if (index >= bases.size())
				throw std::invalid_argument("invalid A64 floating-point rounding mode");
			fp_unary(bases[index], width, rd, rn);
		}
		void fcmp(fp_width width, fp_reg rn, fp_reg rm) {
			check_fp_regs(rn, rm);
			emit_word(fp_width_bit(width) | 0x1e202000u | (uint32_t(rm) << 16) | (uint32_t(rn) << 5));
		}
		void fcmp_zero(fp_width width, fp_reg rn) {
			check_fp_reg(rn);
			emit_word(fp_width_bit(width) | 0x1e202008u | (uint32_t(rn) << 5));
		}
		void fcsel(fp_width width, fp_reg rd, fp_reg rn, fp_reg rm, condition predicate) {
			check_fp_regs(rd, rn, rm);
			check_condition(predicate);
			emit_word(fp_width_bit(width) | 0x1e200c00u | (uint32_t(rm) << 16) | (uint32_t(predicate) << 12) | (uint32_t(rn) << 5) | rd);
		}
		void scvtf(fp_width destination, gp_width source, fp_reg rd, gp_reg rn) {
			check_fp_reg(rd);
			check_reg(rn);
			emit_word(width_bit(source) | fp_width_bit(destination) | 0x1e220000u | (uint32_t(rn) << 5) | rd);
		}
		void ucvtf(fp_width destination, gp_width source, fp_reg rd, gp_reg rn) {
			check_fp_reg(rd);
			check_reg(rn);
			emit_word(width_bit(source) | fp_width_bit(destination) | 0x1e230000u | (uint32_t(rn) << 5) | rd);
		}
		void fcvtzs(gp_width destination, fp_width source, gp_reg rd, fp_reg rn) { fp_to_int(0x1e380000u, destination, source, rd, rn); }
		void fcvtzu(gp_width destination, fp_width source, gp_reg rd, fp_reg rn) { fp_to_int(0x1e390000u, destination, source, rd, rn); }
		void fcvt(fp_width destination, fp_reg rd, fp_width source, fp_reg rn) {
			check_fp_regs(rd, rn);
			if (destination == source) {
				fmov(destination, rd, rn);
			} else if (destination == fp_width::d64 && source == fp_width::s32) {
				emit_word(0x1e22c000u | (uint32_t(rn) << 5) | rd);
			} else if (destination == fp_width::s32 && source == fp_width::d64) {
				emit_word(0x1e624000u | (uint32_t(rn) << 5) | rd);
			} else {
				throw std::invalid_argument("invalid A64 floating-point conversion width");
			}
		}
		void fmov_from_gp(fp_width destination, fp_reg rd, gp_width source, gp_reg rn) {
			check_width(destination);
			check_width(source);
			check_fp_reg(rd);
			check_reg(rn);
			if ((destination == fp_width::s32) != (source == gp_width::w32))
				throw std::invalid_argument("A64 FMOV GP/FP widths must match");
			uint32_t base = destination == fp_width::s32 ? 0x1e270000u : 0x9e670000u;
			emit_word(base | (uint32_t(rn) << 5) | rd);
		}
		void fmov_to_gp(gp_width destination, gp_reg rd, fp_width source, fp_reg rn) {
			check_width(destination);
			check_width(source);
			check_reg(rd);
			check_fp_reg(rn);
			if ((destination == gp_width::w32) != (source == fp_width::s32))
				throw std::invalid_argument("A64 FMOV FP/GP widths must match");
			uint32_t base = source == fp_width::s32 ? 0x1e260000u : 0x9e660000u;
			emit_word(base | (uint32_t(rn) << 5) | rd);
		}

		/// Emits a zero-extending GP load, selecting scaled unsigned or signed unscaled encoding.
		void ldr(memory_size size, gp_reg rt, gp_reg rn, int32_t byte_offset = 0) { emit_word(load_store_imm(true, false, size, rt, rn, byte_offset)); }
		void str(memory_size size, gp_reg rt, gp_reg rn, int32_t byte_offset = 0) { emit_word(load_store_imm(false, false, size, rt, rn, byte_offset)); }
		void ldur(memory_size size, gp_reg rt, gp_reg rn, int32_t byte_offset) { emit_word(load_store_imm(true, true, size, rt, rn, byte_offset)); }
		void stur(memory_size size, gp_reg rt, gp_reg rn, int32_t byte_offset) { emit_word(load_store_imm(false, true, size, rt, rn, byte_offset)); }
		void ldr(memory_size size, gp_reg rt, gp_reg rn, gp_reg rm, index_extend extend, uint32_t amount = 0) {
			emit_word(load_store_reg(true, false, size, rt, rn, rm, extend, amount));
		}
		void str(memory_size size, gp_reg rt, gp_reg rn, gp_reg rm, index_extend extend, uint32_t amount = 0) {
			emit_word(load_store_reg(false, false, size, rt, rn, rm, extend, amount));
		}
		void ldr(fp_width size, fp_reg rt, gp_reg rn, int32_t byte_offset = 0) { emit_word(fp_load_store_imm(true, false, size, rt, rn, byte_offset)); }
		void str(fp_width size, fp_reg rt, gp_reg rn, int32_t byte_offset = 0) { emit_word(fp_load_store_imm(false, false, size, rt, rn, byte_offset)); }
		void ldur(fp_width size, fp_reg rt, gp_reg rn, int32_t byte_offset) { emit_word(fp_load_store_imm(true, true, size, rt, rn, byte_offset)); }
		void stur(fp_width size, fp_reg rt, gp_reg rn, int32_t byte_offset) { emit_word(fp_load_store_imm(false, true, size, rt, rn, byte_offset)); }
		void ldr(fp_width size, fp_reg rt, gp_reg rn, gp_reg rm, index_extend extend, uint32_t amount = 0) {
			emit_word(fp_load_store_reg(true, size, rt, rn, rm, extend, amount));
		}
		void str(fp_width size, fp_reg rt, gp_reg rn, gp_reg rm, index_extend extend, uint32_t amount = 0) {
			emit_word(fp_load_store_reg(false, size, rt, rn, rm, extend, amount));
		}

		void ldp(gp_width width, gp_reg rt, gp_reg rt2, gp_reg rn, int32_t byte_offset, pair_mode mode = pair_mode::offset) {
			check_width(width);
			emit_word(pair(false, true, width == gp_width::x64, rt, rt2, rn, byte_offset, mode));
		}
		void stp(gp_width width, gp_reg rt, gp_reg rt2, gp_reg rn, int32_t byte_offset, pair_mode mode = pair_mode::offset) {
			check_width(width);
			emit_word(pair(false, false, width == gp_width::x64, rt, rt2, rn, byte_offset, mode));
		}
		void ldp(fp_width width, fp_reg rt, fp_reg rt2, gp_reg rn, int32_t byte_offset, pair_mode mode = pair_mode::offset) {
			check_width(width);
			emit_word(pair(true, true, width == fp_width::d64, rt, rt2, rn, byte_offset, mode));
		}
		void stp(fp_width width, fp_reg rt, fp_reg rt2, gp_reg rn, int32_t byte_offset, pair_mode mode = pair_mode::offset) {
			check_width(width);
			emit_word(pair(true, false, width == fp_width::d64, rt, rt2, rn, byte_offset, mode));
		}

		void movz(gp_width width, gp_reg rd, uint32_t immediate, uint32_t shift_amount = 0) {
			emit_word(move_wide(0x52800000u, width, rd, immediate, shift_amount));
		}
		void movk(gp_width width, gp_reg rd, uint32_t immediate, uint32_t shift_amount = 0) {
			emit_word(move_wide(0x72800000u, width, rd, immediate, shift_amount));
		}
		/// Materializes exactly the requested 32- or 64-bit value with MOVZ and only needed MOVKs.
		void mov_constant(gp_width width, gp_reg rd, uint64_t value) {
			check_width(width);
			check_reg(rd);
			unsigned chunks = width == gp_width::x64 ? 4 : 2;
			if (width == gp_width::w32 && value > 0xffffffffu)
				throw std::invalid_argument("A64 W-register constant exceeds 32 bits");
			if (!value) {
				movz(width, rd, 0);
				return;
			}
			unsigned first = 0;
			while (first + 1 < chunks && ((value >> (first * 16)) & 0xffffu) == 0)
				++first;
			movz(width, rd, uint16_t(value >> (first * 16)), uint8_t(first * 16));
			for (unsigned index = 0; index < chunks; ++index) {
				if (index == first)
					continue;
				uint16_t part = uint16_t(value >> (index * 16));
				if (part)
					movk(width, rd, part, uint8_t(index * 16));
			}
		}

		void b(int64_t byte_delta) { emit_word(branch_immediate(false, byte_delta)); }
		void bl(int64_t byte_delta) { emit_word(branch_immediate(true, byte_delta)); }
		void b(condition predicate, int64_t byte_delta) { emit_word(conditional_branch(predicate, byte_delta)); }
		void cbz(gp_width width, gp_reg rt, int64_t byte_delta) { emit_word(compare_branch(false, width, rt, byte_delta)); }
		void cbnz(gp_width width, gp_reg rt, int64_t byte_delta) { emit_word(compare_branch(true, width, rt, byte_delta)); }
		void b(label target) { append_branch(branch_kind::b, target); }
		void bl(label target) { append_branch(branch_kind::bl, target); }
		void b(condition predicate, label target) {
			check_condition(predicate);
			append_branch(branch_kind::conditional, target, predicate);
		}
		void cbz(gp_width width, gp_reg rt, label target) {
			check_reg(rt);
			check_width(width);
			append_branch(branch_kind::cbz, target, condition::eq, width, rt);
		}
		void cbnz(gp_width width, gp_reg rt, label target) {
			check_reg(rt);
			check_width(width);
			append_branch(branch_kind::cbnz, target, condition::eq, width, rt);
		}
		/// Emits an address-independent five-instruction X16 veneer to an absolute address.
		void b_absolute(uint64_t address) { append_absolute_branch(false, address); }
		void bl_absolute(uint64_t address) { append_absolute_branch(true, address); }
		void br(gp_reg rn) {
			check_branch_reg(rn);
			emit_word(0xd61f0000u | (uint32_t(rn) << 5));
		}
		void blr(gp_reg rn) {
			check_branch_reg(rn);
			emit_word(0xd63f0000u | (uint32_t(rn) << 5));
		}
		void ret(gp_reg rn = 30) {
			check_branch_reg(rn);
			emit_word(0xd65f0000u | (uint32_t(rn) << 5));
		}
		void brk(uint32_t immediate = 0) {
			if (immediate > 0xffff)
				throw std::invalid_argument("A64 BRK immediate exceeds 16 bits");
			emit_word(0xd4200000u | (immediate << 5));
		}

		/// Adds immutable data to the final guarded literal pool.
		literal literal32(uint64_t value) {
			if (value > 0xffffffffu)
				throw std::invalid_argument("A64 32-bit literal value exceeds 32 bits");
			return add_literal(value, 4, literal_kind::raw);
		}
		literal literal64(uint64_t value) { return add_literal(value, 8, literal_kind::raw); }
		/// Adds a relocated absolute address of an internal label to the literal pool.
		literal address_literal(label target) {
			check_label(target);
			literal result              = add_literal(0, 8, literal_kind::label_address);
			literals_[result.id].target = target;
			return result;
		}
		void ldr_literal(gp_width width, gp_reg rt, literal source) {
			check_width(width);
			check_reg(rt);
			if (rt == 31)
				throw std::invalid_argument("A64 long literal loads cannot target ZR");
			check_literal(source, width == gp_width::x64 ? 8 : 4);
			item value;
			value.kind        = item_kind::gp_literal;
			value.literal_ref = source;
			value.gp_size     = width;
			value.reg         = rt;
			append_item(value);
		}
		void ldr_literal(fp_width width, fp_reg rt, literal source) {
			check_width(width);
			check_fp_reg(rt);
			check_literal(source, width == fp_width::d64 ? 8 : 4);
			item value;
			value.kind        = item_kind::fp_literal;
			value.literal_ref = source;
			value.fp_size     = width;
			value.reg         = rt;
			append_item(value);
		}

		/// Resolves labels, relaxes branches, builds the literal pool, and applies internal relocations.
		/// load_address is the address at which byte zero will execute; executable mappings are page aligned.
		void finish(uintptr_t load_address = 0) {
			if (!dirty_ && finalized_address_ == load_address)
				return;
			for (bool bound : labels_) {
				if (!bound)
					throw std::logic_error("A64 emitter contains an unbound label");
			}

			std::vector<uint64_t> sizes(items_.size(), 0);
			for (size_t index = 0; index < items_.size(); ++index) {
				switch (items_[index].kind) {
					case item_kind::word:
						sizes[index] = 1;
						break;
					case item_kind::padding:
						sizes[index] = items_[index].count;
						break;
					case item_kind::label:
						sizes[index] = 0;
						break;
					case item_kind::absolute_branch:
						sizes[index] = 5;
						break;
					default:
						sizes[index] = 1;
						break;
				}
			}

			std::vector<uint64_t> item_offsets(items_.size());
			std::vector<uint64_t> label_offsets(labels_.size());
			std::vector<uint64_t> literal_offsets(literals_.size());
			uint64_t              code_words = 0;
			uint64_t              end_words  = 0;
			auto                  layout     = [&]() {
				uint64_t cursor = 0;
				for (size_t index = 0; index < items_.size(); ++index) {
					item_offsets[index] = cursor;
					if (items_[index].kind == item_kind::label)
						label_offsets[items_[index].target.id] = cursor;
					cursor += sizes[index];
				}
				code_words = cursor;
				if (!literals_.empty())
					++cursor;  // Branch around the pool if normal control flow reaches its end.
				for (size_t index = 0; index < literals_.size(); ++index) {
					if (literals_[index].size == 8 && (cursor & 1u))
						++cursor;
					literal_offsets[index] = cursor;
					cursor += literals_[index].size / 4;
				}
				end_words = cursor;
			};

			bool changed;
			do {
				changed = false;
				layout();
				for (size_t index = 0; index < items_.size(); ++index) {
					const item& value  = items_[index];
					uint64_t    source = item_offsets[index] * 4;
					uint64_t    wanted = sizes[index];
					if (value.kind == item_kind::branch) {
						int64_t target = checked_delta(label_offsets[value.target.id] * 4, source);
						if (value.branch == branch_kind::b || value.branch == branch_kind::bl) {
							wanted = fits_signed_scaled(target, 26) ? 1 : 3;
						} else {
							wanted =
								 fits_signed_scaled(target, 19) ? 1 : (fits_signed_scaled(checked_delta(label_offsets[value.target.id] * 4, source + 4), 26) ? 2 : 4);
						}
					} else if (value.kind == item_kind::gp_literal || value.kind == item_kind::fp_literal) {
						int64_t target = checked_delta(literal_offsets[value.literal_ref.id] * 4, source);
						wanted         = fits_signed_scaled(target, 19) ? 1 : 2;
					}
					if (wanted > sizes[index]) {
						sizes[index] = wanted;
						changed      = true;
					}
				}
			} while (changed);
			layout();

			if (end_words > std::numeric_limits<size_t>::max() / 4)
				throw std::length_error("A64 output exceeds addressable memory");
			words_.clear();
			bytes_.clear();
			relocations_.clear();
			words_.reserve(static_cast<size_t>(end_words));

			for (size_t index = 0; index < items_.size(); ++index) {
				const item& value       = items_[index];
				uint64_t    source_word = words_.size();
				switch (value.kind) {
					case item_kind::word:
						words_.push_back(value.word);
						break;
					case item_kind::padding:
						for (uint64_t count = 0; count < value.count; ++count)
							words_.push_back(0xd503201f);
						break;
					case item_kind::label:
						break;
					case item_kind::branch:
						emit_relaxed_branch(value, sizes[index], source_word, label_offsets[value.target.id], load_address);
						break;
					case item_kind::absolute_branch:
						emit_absolute_veneer(value, source_word);
						break;
					case item_kind::gp_literal:
					case item_kind::fp_literal:
						emit_literal_load(value, sizes[index], source_word, literal_offsets[value.literal_ref.id], load_address);
						break;
				}
			}

			if (!literals_.empty()) {
				int64_t skip = checked_delta(end_words * 4, code_words * 4);
				words_.push_back(branch_immediate(false, skip));
				for (size_t index = 0; index < literals_.size(); ++index) {
					while (words_.size() < literal_offsets[index])
						words_.push_back(0);
					uint64_t value = literals_[index].value;
					if (literals_[index].kind == literal_kind::label_address) {
						value = checked_address(load_address, label_offsets[literals_[index].target.id] * 4);
						add_relocation(relocation_kind::absolute64, words_.size(), label_target(literals_[index].target));
					}
					words_.push_back(uint32_t(value));
					if (literals_[index].size == 8)
						words_.push_back(uint32_t(value >> 32));
				}
			}
			if (words_.size() != end_words)
				throw std::logic_error("A64 relaxation produced an inconsistent layout");

			bytes_.reserve(words_.size() * 4);
			for (uint32_t word : words_) {
				bytes_.push_back(uint8_t(word));
				bytes_.push_back(uint8_t(word >> 8));
				bytes_.push_back(uint8_t(word >> 16));
				bytes_.push_back(uint8_t(word >> 24));
			}
			finalized_address_ = load_address;
			dirty_             = false;
		}

		/// Host-endian integer values of each encoded little-endian instruction/data word.
		std::span<const uint32_t> words(uintptr_t load_address = 0) {
			finish(load_address);
			return words_;
		}
		/// Canonical little-endian byte stream suitable for copying into executable memory.
		std::span<const uint8_t> bytes(uintptr_t load_address = 0) {
			finish(load_address);
			return bytes_;
		}
		/// Typed relocation metadata for the most recently finalized stream.
		std::span<const relocation> relocations(uintptr_t load_address = 0) {
			finish(load_address);
			return relocations_;
		}

	  private:
		enum class item_kind : uint8_t { word, padding, label, branch, absolute_branch, gp_literal, fp_literal };
		enum class branch_kind : uint8_t { b, bl, conditional, cbz, cbnz };
		enum class literal_kind : uint8_t { raw, label_address };
		struct literal_entry {
			uint64_t     value  = 0;
			uint8_t      size   = 0;
			literal_kind kind   = literal_kind::raw;
			label        target = {};
		};
		struct item {
			item_kind   kind        = item_kind::word;
			uint32_t    word        = 0;
			uint64_t    count       = 0;
			branch_kind branch      = branch_kind::b;
			condition   predicate   = condition::eq;
			gp_width    gp_size     = gp_width::x64;
			fp_width    fp_size     = fp_width::d64;
			gp_reg      reg         = 0;
			label       target      = {};
			literal     literal_ref = {};
			uint64_t    address     = 0;
		};

		std::vector<item>          items_;
		std::vector<bool>          labels_;
		std::vector<literal_entry> literals_;
		std::vector<uint32_t>      words_;
		std::vector<uint8_t>       bytes_;
		std::vector<relocation>    relocations_;
		uintptr_t                  finalized_address_ = 0;
		bool                       dirty_             = true;

		static void check_reg(gp_reg reg) {
			if (reg > 31)
				throw std::invalid_argument("A64 GP register ID must be in 0..31");
		}
		template<typename... Rest>
		static void check_regs(gp_reg first, Rest... rest) {
			check_reg(first);
			(check_reg(rest), ...);
		}
		static void check_fp_reg(fp_reg reg) {
			if (reg > 31)
				throw std::invalid_argument("A64 FP register ID must be in 0..31");
		}
		template<typename... Rest>
		static void check_fp_regs(fp_reg first, Rest... rest) {
			check_fp_reg(first);
			(check_fp_reg(rest), ...);
		}
		static void check_width(gp_width width) {
			if (width != gp_width::w32 && width != gp_width::x64)
				throw std::invalid_argument("invalid A64 GP width");
		}
		static void check_width(fp_width width) {
			if (width != fp_width::s32 && width != fp_width::d64)
				throw std::invalid_argument("invalid A64 FP width");
		}
		static uint32_t width_bit(gp_width width) {
			check_width(width);
			return width == gp_width::x64 ? 0x80000000u : 0;
		}
		static uint32_t fp_width_bit(fp_width width) {
			check_width(width);
			return width == fp_width::d64 ? 0x00400000u : 0;
		}
		static uint8_t width_bits(gp_width width) {
			check_width(width);
			return width == gp_width::x64 ? 64 : 32;
		}
		static void check_condition(condition predicate) {
			if (static_cast<uint8_t>(predicate) > static_cast<uint8_t>(condition::le))
				throw std::invalid_argument("A64 predicate must be a conditional code EQ..LE");
		}
		static void check_shift(gp_width width, shift kind, uint32_t amount) {
			check_width(width);
			if (static_cast<uint8_t>(kind) > 3 || amount >= width_bits(width))
				throw std::invalid_argument("invalid A64 shift kind or amount");
		}
		static void check_branch_reg(gp_reg reg) {
			check_reg(reg);
			if (reg == 31)
				throw std::invalid_argument("A64 indirect branch register cannot be XZR");
		}

		void check_label(label value) const {
			if (value.owner != this || value.id >= labels_.size())
				throw std::invalid_argument("A64 label belongs to another emitter");
		}
		void check_literal(literal value, uint8_t size) const {
			if (value.owner != this || value.id >= literals_.size())
				throw std::invalid_argument("A64 literal belongs to another emitter");
			if (literals_[value.id].size != size)
				throw std::invalid_argument("A64 literal width does not match its load");
		}
		void append_item(const item& value) {
			items_.push_back(value);
			dirty_ = true;
		}
		literal add_literal(uint64_t value, uint8_t size, literal_kind kind) {
			literals_.push_back({value, size, kind, {}});
			dirty_ = true;
			return {static_cast<uint32_t>(literals_.size() - 1), this};
		}

		static uint32_t add_sub_reg(bool subtract, bool set_flags, gp_width width, gp_reg rd, gp_reg rn, gp_reg rm, shift kind, uint32_t amount) {
			check_regs(rd, rn, rm);
			check_shift(width, kind, amount);
			if (kind == shift::ror)
				throw std::invalid_argument("A64 ADD/SUB shifted register does not support ROR");
			uint32_t base = width_bit(width) | 0x0b000000u;
			if (subtract)
				base |= 0x40000000u;
			if (set_flags)
				base |= 0x20000000u;
			return base | (uint32_t(kind) << 22) | (uint32_t(rm) << 16) | (uint32_t(amount) << 10) | (uint32_t(rn) << 5) | rd;
		}
		static uint32_t add_sub_imm(bool subtract, bool set_flags, gp_width width, gp_reg rd, gp_reg rn, uint32_t immediate, uint32_t shift_amount) {
			check_regs(rd, rn);
			check_width(width);
			if (immediate > 4095 || (shift_amount != 0 && shift_amount != 12))
				throw std::invalid_argument("A64 ADD/SUB immediate is imm12 optionally shifted by 12");
			uint32_t base = width_bit(width) | 0x11000000u;
			if (subtract)
				base |= 0x40000000u;
			if (set_flags)
				base |= 0x20000000u;
			return base | (uint32_t(shift_amount == 12) << 22) | (uint32_t(immediate) << 10) | (uint32_t(rn) << 5) | rd;
		}
		static uint32_t logical_reg(uint8_t opcode, gp_width width, gp_reg rd, gp_reg rn, gp_reg rm, shift kind, uint32_t amount) {
			check_regs(rd, rn, rm);
			check_shift(width, kind, amount);
			if (opcode > 3)
				throw std::invalid_argument("invalid A64 logical opcode");
			return width_bit(width) | 0x0a000000u | (uint32_t(opcode) << 29) | (uint32_t(kind) << 22) | (uint32_t(rm) << 16) | (uint32_t(amount) << 10) |
					 (uint32_t(rn) << 5) | rd;
		}
		static uint64_t rotate_right(uint64_t value, unsigned amount, unsigned bits) {
			uint64_t mask = bits == 64 ? ~uint64_t{0} : ((uint64_t{1} << bits) - 1);
			amount &= bits - 1;
			value &= mask;
			if (!amount)
				return value;
			return ((value >> amount) | (value << (bits - amount))) & mask;
		}
		static uint64_t repeat_element(uint64_t element, unsigned element_bits, unsigned total_bits) {
			uint64_t result = 0;
			for (unsigned offset = 0; offset < total_bits; offset += element_bits)
				result |= element << offset;
			return result;
		}
		static uint32_t logical_fields(gp_width width, uint64_t immediate) {
			unsigned bits = width_bits(width);
			if (bits == 32 && immediate > 0xffffffffu)
				throw std::invalid_argument("A64 W-register logical immediate exceeds 32 bits");
			uint64_t full = bits == 64 ? ~uint64_t{0} : 0xffffffffu;
			if (immediate == 0 || immediate == full)
				throw std::invalid_argument("A64 logical immediate cannot be zero or all ones");
			for (unsigned element_bits = 2; element_bits <= bits; element_bits <<= 1) {
				uint64_t element_mask = element_bits == 64 ? ~uint64_t{0} : ((uint64_t{1} << element_bits) - 1);
				uint64_t element      = immediate & element_mask;
				if (repeat_element(element, element_bits, bits) != immediate)
					continue;
				for (unsigned ones = 1; ones < element_bits; ++ones) {
					uint64_t run = (uint64_t{1} << ones) - 1;
					for (unsigned rotation = 0; rotation < element_bits; ++rotation) {
						if (rotate_right(run, rotation, element_bits) != element)
							continue;
						uint32_t n    = element_bits == 64 ? 1u : 0u;
						uint32_t immr = rotation & 63u;
						uint32_t imms = ((~(element_bits - 1u) << 1u) | (ones - 1u)) & 63u;
						return (n << 22) | (immr << 16) | (imms << 10);
					}
				}
			}
			throw std::invalid_argument("value is not encodable as an A64 logical immediate");
		}
		static uint32_t logical_imm(uint8_t opcode, gp_width width, gp_reg rd, gp_reg rn, uint64_t immediate) {
			check_regs(rd, rn);
			if (opcode > 3)
				throw std::invalid_argument("invalid A64 logical immediate opcode");
			return width_bit(width) | 0x12000000u | (uint32_t(opcode) << 29) | logical_fields(width, immediate) | (uint32_t(rn) << 5) | rd;
		}
		void bitfield_shift(gp_width width, gp_reg rd, gp_reg rn, uint32_t amount, shift kind) {
			check_regs(rd, rn);
			unsigned bits = width_bits(width);
			if (amount >= bits)
				throw std::invalid_argument("A64 immediate shift exceeds operand width");
			uint32_t base = kind == shift::asr ? 0x13000000u : 0x53000000u;
			if (width == gp_width::x64)
				base |= 0x80400000u;
			uint32_t immr;
			uint32_t imms;
			if (kind == shift::lsl) {
				immr = (-amount) & (bits - 1);
				imms = bits - 1 - amount;
			} else if (kind == shift::lsr || kind == shift::asr) {
				immr = amount;
				imms = bits - 1;
			} else {
				throw std::invalid_argument("invalid A64 bitfield shift");
			}
			emit_word(base | (immr << 16) | (imms << 10) | (uint32_t(rn) << 5) | rd);
		}
		void variable_shift(gp_width width, gp_reg rd, gp_reg rn, gp_reg rm, uint8_t opcode) {
			check_regs(rd, rn, rm);
			check_width(width);
			emit_word(width_bit(width) | 0x1ac02000u | (uint32_t(opcode) << 10) | (uint32_t(rm) << 16) | (uint32_t(rn) << 5) | rd);
		}
		void signed_extend(gp_width width, gp_reg rd, gp_reg rn, uint32_t source_bits) {
			check_regs(rd, rn);
			check_width(width);
			uint32_t destination_bits = width_bits(width);
			if (source_bits == 0 || source_bits > destination_bits)
				throw std::invalid_argument("A64 signed extension source exceeds destination width");
			uint32_t base = width == gp_width::x64 ? 0x93400000u : 0x13000000u;
			emit_word(base | ((source_bits - 1) << 10) | (uint32_t(rn) << 5) | rd);
		}
		void fp_unary(uint32_t base, fp_width width, fp_reg rd, fp_reg rn) {
			check_fp_regs(rd, rn);
			emit_word(base | fp_width_bit(width) | (uint32_t(rn) << 5) | rd);
		}
		void fp_binary(uint32_t base, fp_width width, fp_reg rd, fp_reg rn, fp_reg rm) {
			check_fp_regs(rd, rn, rm);
			emit_word(base | fp_width_bit(width) | (uint32_t(rm) << 16) | (uint32_t(rn) << 5) | rd);
		}
		void fp_to_int(uint32_t base, gp_width destination, fp_width source, gp_reg rd, fp_reg rn) {
			check_reg(rd);
			check_fp_reg(rn);
			emit_word(base | width_bit(destination) | fp_width_bit(source) | (uint32_t(rn) << 5) | rd);
		}

		static uint32_t size_bytes(memory_size size) {
			uint8_t value = static_cast<uint8_t>(size);
			if (value > 3)
				throw std::invalid_argument("invalid A64 memory operand size");
			return 1u << value;
		}
		static uint32_t load_store_imm(bool load, bool force_unscaled, memory_size size, gp_reg rt, gp_reg rn, int32_t byte_offset) {
			check_regs(rt, rn);
			uint32_t scale     = size_bytes(size);
			uint32_t size_code = static_cast<uint8_t>(size);
			if (!force_unscaled && byte_offset >= 0 && byte_offset % int32_t(scale) == 0 && uint32_t(byte_offset / int32_t(scale)) <= 4095) {
				return 0x39000000u | (size_code << 30) | (load ? 0x00400000u : 0) | (uint32_t(byte_offset / int32_t(scale)) << 10) | (uint32_t(rn) << 5) | rt;
			}
			if (byte_offset < -256 || byte_offset > 255)
				throw std::invalid_argument("A64 load/store offset is outside scaled imm12 and unscaled imm9 ranges");
			return 0x38000000u | (size_code << 30) | (load ? 0x00400000u : 0) | ((uint32_t(byte_offset) & 0x1ffu) << 12) | (uint32_t(rn) << 5) | rt;
		}
		static uint32_t load_store_reg(bool load, bool fp, memory_size size, uint8_t rt, gp_reg rn, gp_reg rm, index_extend extend, uint32_t amount) {
			if (fp)
				check_fp_reg(rt);
			else
				check_reg(rt);
			check_regs(rn, rm);
			uint32_t scale        = size_bytes(size);
			uint8_t  scaled_shift = static_cast<uint8_t>(size);
			uint8_t  option       = static_cast<uint8_t>(extend);
			if (option != 2 && option != 3 && option != 6 && option != 7)
				throw std::invalid_argument("invalid A64 register-offset extension");
			if (amount != 0 && amount != scaled_shift)
				throw std::invalid_argument("A64 register offset shift must be zero or log2(access size)");
			(void) scale;
			uint32_t base = (static_cast<uint32_t>(size) << 30) | (fp ? 0x3c200800u : 0x38200800u);
			if (load)
				base |= 0x00400000u;
			return base | (uint32_t(rm) << 16) | (uint32_t(option) << 13) | (uint32_t(amount != 0) << 12) | (uint32_t(rn) << 5) | rt;
		}
		static uint32_t fp_load_store_imm(bool load, bool force_unscaled, fp_width width, fp_reg rt, gp_reg rn, int32_t byte_offset) {
			check_fp_reg(rt);
			check_reg(rn);
			check_width(width);
			uint32_t scale = width == fp_width::d64 ? 8 : 4;
			if (!force_unscaled && byte_offset >= 0 && byte_offset % int32_t(scale) == 0 && uint32_t(byte_offset / int32_t(scale)) <= 4095) {
				return (width == fp_width::d64 ? 0xfd000000u : 0xbd000000u) | (load ? 0x00400000u : 0) | (uint32_t(byte_offset / int32_t(scale)) << 10) |
						 (uint32_t(rn) << 5) | rt;
			}
			if (byte_offset < -256 || byte_offset > 255)
				throw std::invalid_argument("A64 FP load/store offset is outside scaled imm12 and unscaled imm9 ranges");
			return (width == fp_width::d64 ? 0xfc000000u : 0xbc000000u) | (load ? 0x00400000u : 0) | ((uint32_t(byte_offset) & 0x1ffu) << 12) |
					 (uint32_t(rn) << 5) | rt;
		}
		static uint32_t fp_load_store_reg(bool load, fp_width width, fp_reg rt, gp_reg rn, gp_reg rm, index_extend extend, uint32_t amount) {
			check_width(width);
			return load_store_reg(load, true, width == fp_width::d64 ? memory_size::u64 : memory_size::u32, rt, rn, rm, extend, amount);
		}
		static uint32_t pair(bool fp, bool load, bool wide, uint8_t rt, uint8_t rt2, gp_reg rn, int32_t byte_offset, pair_mode mode) {
			if (fp)
				check_fp_regs(rt, rt2);
			else
				check_regs(rt, rt2);
			check_reg(rn);
			uint32_t scale = wide ? 8 : 4;
			if (byte_offset % int32_t(scale) != 0 || byte_offset / int32_t(scale) < -64 || byte_offset / int32_t(scale) > 63)
				throw std::invalid_argument("A64 pair load/store offset must be aligned and fit signed imm7");
			if (mode != pair_mode::offset && mode != pair_mode::pre_index && mode != pair_mode::post_index)
				throw std::invalid_argument("invalid A64 pair addressing mode");
			if (mode != pair_mode::offset && rn != 31 && (rn == rt || rn == rt2))
				throw std::invalid_argument("A64 pair writeback base cannot overlap a transferred register");
			uint32_t base;
			if (fp)
				base = wide ? 0x6d000000u : 0x2d000000u;
			else
				base = wide ? 0xa9000000u : 0x29000000u;
			if (load)
				base |= 0x00400000u;
			if (mode == pair_mode::pre_index)
				base |= 0x00800000u;
			else if (mode == pair_mode::post_index)
				base -= 0x00800000u;
			return base | ((uint32_t(byte_offset / int32_t(scale)) & 0x7fu) << 15) | (uint32_t(rt2) << 10) | (uint32_t(rn) << 5) | rt;
		}
		static uint32_t move_wide(uint32_t base, gp_width width, gp_reg rd, uint32_t immediate, uint32_t shift_amount) {
			check_reg(rd);
			check_width(width);
			if (immediate > 0xffff)
				throw std::invalid_argument("A64 MOVZ/MOVK immediate exceeds 16 bits");
			if (shift_amount % 16 != 0 || shift_amount >= width_bits(width))
				throw std::invalid_argument("A64 MOVZ/MOVK shift must be a valid multiple of 16");
			return width_bit(width) | base | (uint32_t(shift_amount / 16) << 21) | (uint32_t(immediate) << 5) | rd;
		}

		static bool fits_signed_scaled(int64_t byte_delta, unsigned bits) {
			if ((byte_delta & 3) != 0)
				return false;
			int64_t words = byte_delta / 4;
			int64_t low   = -(int64_t{1} << (bits - 1));
			int64_t high  = (int64_t{1} << (bits - 1)) - 1;
			return low <= words && words <= high;
		}
		static uint32_t branch_immediate(bool link, int64_t byte_delta) {
			if (!fits_signed_scaled(byte_delta, 26))
				throw std::invalid_argument("A64 B/BL displacement must be 4-byte aligned and fit signed imm26");
			return (link ? 0x94000000u : 0x14000000u) | (uint32_t(byte_delta / 4) & 0x03ffffffu);
		}
		static uint32_t conditional_branch(condition predicate, int64_t byte_delta) {
			check_condition(predicate);
			if (!fits_signed_scaled(byte_delta, 19))
				throw std::invalid_argument("A64 B.cond displacement must be 4-byte aligned and fit signed imm19");
			return 0x54000000u | ((uint32_t(byte_delta / 4) & 0x7ffffu) << 5) | uint32_t(predicate);
		}
		static uint32_t compare_branch(bool nonzero, gp_width width, gp_reg rt, int64_t byte_delta) {
			check_reg(rt);
			if (!fits_signed_scaled(byte_delta, 19))
				throw std::invalid_argument("A64 CBZ/CBNZ displacement must be 4-byte aligned and fit signed imm19");
			return width_bit(width) | (nonzero ? 0x35000000u : 0x34000000u) | ((uint32_t(byte_delta / 4) & 0x7ffffu) << 5) | rt;
		}
		static uint32_t adrp(gp_reg rd, uint64_t target_address, uint64_t source_address) {
			check_reg(rd);
			int64_t pages = checked_delta(target_address >> 12, source_address >> 12);
			if (pages < -(int64_t{1} << 20) || pages > ((int64_t{1} << 20) - 1))
				throw std::out_of_range("A64 ADRP veneer target is outside signed 21-bit page reach");
			uint64_t encoded = uint64_t(pages) & 0x1fffffu;
			return 0x90000000u | (uint32_t(encoded & 3u) << 29) | (uint32_t((encoded >> 2) & 0x7ffffu) << 5) | rd;
		}
		static int64_t checked_delta(uint64_t target, uint64_t source) {
			if (target >= source) {
				uint64_t value = target - source;
				if (value > uint64_t(std::numeric_limits<int64_t>::max()))
					throw std::overflow_error("A64 displacement exceeds int64 range");
				return int64_t(value);
			}
			uint64_t value = source - target;
			if (value > uint64_t(std::numeric_limits<int64_t>::max()))
				throw std::overflow_error("A64 displacement exceeds int64 range");
			return -int64_t(value);
		}
		static uint64_t checked_address(uintptr_t base, uint64_t offset) {
			if (offset > std::numeric_limits<uintptr_t>::max() - base)
				throw std::overflow_error("A64 code address overflows uintptr_t");
			return uint64_t(base) + offset;
		}

		void append_branch(branch_kind kind, label target, condition predicate = condition::eq, gp_width width = gp_width::x64, gp_reg reg = 0) {
			check_label(target);
			item value;
			value.kind      = item_kind::branch;
			value.branch    = kind;
			value.target    = target;
			value.predicate = predicate;
			value.gp_size   = width;
			value.reg       = reg;
			append_item(value);
		}
		void append_absolute_branch(bool link, uint64_t address) {
			item value;
			value.kind    = item_kind::absolute_branch;
			value.branch  = link ? branch_kind::bl : branch_kind::b;
			value.address = address;
			append_item(value);
		}
		static relocation_target label_target(label target) { return {relocation_target_kind::internal_label, target.id, 0}; }
		static relocation_target literal_target(literal target) { return {relocation_target_kind::literal_pool, target.id, 0}; }
		static relocation_target absolute_target(uint64_t target) { return {relocation_target_kind::absolute_address, 0, target}; }
		void                     add_relocation(relocation_kind kind, uint64_t word_offset, relocation_target target, int64_t addend = 0) {
			relocations_.push_back({kind, word_offset * 4, target, addend});
		}
		void emit_relaxed_branch(const item& value, uint64_t size, uint64_t source_word, uint64_t target_word, uintptr_t load_address) {
			int64_t delta  = checked_delta(target_word * 4, source_word * 4);
			auto    target = label_target(value.target);
			if (size == 1) {
				if (value.branch == branch_kind::b || value.branch == branch_kind::bl) {
					words_.push_back(branch_immediate(value.branch == branch_kind::bl, delta));
					add_relocation(relocation_kind::branch26, source_word, target);
				} else if (value.branch == branch_kind::conditional) {
					words_.push_back(conditional_branch(value.predicate, delta));
					add_relocation(relocation_kind::conditional19, source_word, target);
				} else {
					words_.push_back(compare_branch(value.branch == branch_kind::cbnz, value.gp_size, value.reg, delta));
					add_relocation(relocation_kind::compare_branch19, source_word, target);
				}
				return;
			}
			if (value.branch == branch_kind::conditional) {
				words_.push_back(conditional_branch(invert(value.predicate), size == 2 ? 8 : 16));
			} else if (value.branch == branch_kind::cbz || value.branch == branch_kind::cbnz) {
				words_.push_back(compare_branch(value.branch == branch_kind::cbz, value.gp_size, value.reg, size == 2 ? 8 : 16));
			}
			if (size == 2) {
				uint64_t branch_word = source_word + 1;
				words_.push_back(branch_immediate(false, checked_delta(target_word * 4, branch_word * 4)));
				add_relocation(relocation_kind::branch26, branch_word, target);
				return;
			}
			uint64_t veneer_word = source_word + ((value.branch == branch_kind::b || value.branch == branch_kind::bl) ? 0 : 1);
			uint64_t source_addr = checked_address(load_address, veneer_word * 4);
			uint64_t target_addr = checked_address(load_address, target_word * 4);
			words_.push_back(adrp(16, target_addr, source_addr));
			add_relocation(relocation_kind::adr_page21, veneer_word, target);
			words_.push_back(add_sub_imm(false, false, gp_width::x64, 16, 16, uint16_t(target_addr & 0xfff), 0));
			add_relocation(relocation_kind::add_pageoff12, veneer_word + 1, target);
			words_.push_back((value.branch == branch_kind::bl ? 0xd63f0000u : 0xd61f0000u) | (16u << 5));
		}
		void emit_absolute_veneer(const item& value, uint64_t source_word) {
			for (unsigned chunk = 0; chunk < 4; ++chunk) {
				uint16_t part = uint16_t(value.address >> (chunk * 16));
				words_.push_back(move_wide(chunk ? 0x72800000u : 0x52800000u, gp_width::x64, 16, part, uint8_t(chunk * 16)));
				add_relocation(
					 static_cast<relocation_kind>(static_cast<uint8_t>(relocation_kind::movw_uabs_g0) + chunk), source_word + chunk, absolute_target(value.address));
			}
			words_.push_back((value.branch == branch_kind::bl ? 0xd63f0000u : 0xd61f0000u) | (16u << 5));
		}
		void emit_literal_load(const item& value, uint64_t size, uint64_t source_word, uint64_t target_word, uintptr_t load_address) {
			int64_t delta  = checked_delta(target_word * 4, source_word * 4);
			auto    target = literal_target(value.literal_ref);
			if (size == 1) {
				uint32_t base;
				if (value.kind == item_kind::gp_literal)
					base = value.gp_size == gp_width::x64 ? 0x58000000u : 0x18000000u;
				else
					base = value.fp_size == fp_width::d64 ? 0x5c000000u : 0x1c000000u;
				words_.push_back(base | ((uint32_t(delta / 4) & 0x7ffffu) << 5) | value.reg);
				add_relocation(relocation_kind::literal19, source_word, target);
				return;
			}
			gp_reg   scratch     = value.kind == item_kind::gp_literal ? value.reg : 16;
			uint64_t source_addr = checked_address(load_address, source_word * 4);
			uint64_t target_addr = checked_address(load_address, target_word * 4);
			words_.push_back(adrp(scratch, target_addr, source_addr));
			add_relocation(relocation_kind::adr_page21, source_word, target);
			if (value.kind == item_kind::gp_literal) {
				memory_size mem = value.gp_size == gp_width::x64 ? memory_size::u64 : memory_size::u32;
				words_.push_back(load_store_imm(true, false, mem, value.reg, scratch, int32_t(target_addr & 0xfff)));
			} else {
				words_.push_back(fp_load_store_imm(true, false, value.fp_size, value.reg, scratch, int32_t(target_addr & 0xfff)));
			}
			add_relocation(relocation_kind::load_pageoff12, source_word + 1, target);
		}
	};
}
