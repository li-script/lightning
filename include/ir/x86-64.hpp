#pragma once
#include <util/common.hpp>
#if LI_JIT && LI_ARCH_X86 && !LI_32
#include <ir/mir.hpp>

#if LI_CLANG
	#pragma clang diagnostic ignored "-Wunneeded-internal-declaration"
	#pragma clang diagnostic ignored "-Wunused-function"
	#pragma clang diagnostic ignored "-Wunused-const-variable"
	#pragma clang diagnostic ignored "-Winvalid-offsetof"
	#pragma clang diagnostic ignored "-Wswitch"
#endif

// Compiler options.
//
#if __AVX__
	static constexpr bool USE_AVX = true;
#else
	static constexpr bool USE_AVX = false;
#endif
static constexpr size_t BRANCH_ALIGN = 16; // TODO:
static constexpr size_t MAX_NOP_LENGTH = 15;
// clang-format off
static constexpr uint8_t NOP_TABLE[MAX_NOP_LENGTH][MAX_NOP_LENGTH] = {
	{ 0x90 },
	{ 0x66, 0x90 },
	{ 0x0F, 0x1F, 0x00 },
	{ 0x0F, 0x1F, 0x40, 0x00 },
	{ 0x0F, 0x1F, 0x44, 0x00, 0x00 },
	{ 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 },
	{ 0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00 },
	{ 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0x66, 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0x66, 0x66, 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0x66, 0x66, 0x66, 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0x66, 0x66, 0x66, 0x66, 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 }
};
// clang-format on

namespace li::ir {
	// Flags.
	//
	struct flag_info {
		arch::native_mnemonic js;
		arch::native_mnemonic jns;
		arch::native_mnemonic sets;
		arch::native_mnemonic setns;
		arch::native_mnemonic cmovs;
		arch::native_mnemonic cmovns;
	};
	#define ENUM_FLAGS(_) _(Z, 0) _(S, 2) _(B, 4) _(BE, 6) _(L, 8) _(LE, 10) _(O, 12) _(P, 14)
	inline static constexpr flag_info flags[] = {
	#define ENUMERATOR(F, id) \
	 flag_info{LI_STRCAT(ZYDIS_MNEMONIC_J, F), LI_STRCAT(ZYDIS_MNEMONIC_JN, F), LI_STRCAT(ZYDIS_MNEMONIC_SET, F), LI_STRCAT(ZYDIS_MNEMONIC_SETN, F), LI_STRCAT(ZYDIS_MNEMONIC_CMOV, F), LI_STRCAT(ZYDIS_MNEMONIC_CMOVN, F)}, \
	 flag_info{LI_STRCAT(ZYDIS_MNEMONIC_JN, F), LI_STRCAT(ZYDIS_MNEMONIC_J, F), LI_STRCAT(ZYDIS_MNEMONIC_SETN, F), LI_STRCAT(ZYDIS_MNEMONIC_SET, F), LI_STRCAT(ZYDIS_MNEMONIC_CMOVN, F), LI_STRCAT(ZYDIS_MNEMONIC_CMOV, F)},
		 ENUM_FLAGS(ENUMERATOR)
	#undef ENUMERATOR
	};
	#define ENUMERATOR(F, id)                                       \
		static constexpr flag_id LI_STRCAT(FLAG_, F)  = flag_id(id); \
		static constexpr flag_id LI_STRCAT(FLAG_N, F) = flag_id(id + 1);
	ENUM_FLAGS(ENUMERATOR)
	#undef ENUMERATOR
	#undef ENUM_FLAGS

	// Instructions.
	//
	enum encoding_directive : msize_t {
		ENC_NOP,
		ENC_W_R,
		ENC_RW_R,
		ENC_RW,
		ENC_W_R_R,
		ENC_W_N_R_R,
		ENC_F_R_R,
	};
	
	#define INSN_NOP(name, ...) \
		static minsn& name(mblock& blk) { return blk.instructions.emplace_back(minsn{LI_STRCAT(ZYDIS_MNEMONIC_, name), {__VA_ARGS__.rsvd = ENC_NOP}, {}}); }
	#define INSN_W_R(name, ...) \
		static minsn& name(mblock& blk, mreg a, mop b) { return blk.instructions.emplace_back(minsn{LI_STRCAT(ZYDIS_MNEMONIC_, name), {__VA_ARGS__.rsvd = ENC_W_R}, a, b}); }
	#define INSN_RW_R(name, ...) \
		static minsn& name(mblock& blk, mreg a, mop b) { return blk.instructions.emplace_back(minsn{LI_STRCAT(ZYDIS_MNEMONIC_, name), {__VA_ARGS__.rsvd = ENC_RW_R}, a, a, b}); }
	#define INSN_RW(name, ...) \
		static minsn& name(mblock& blk, mreg a) { return blk.instructions.emplace_back(minsn{LI_STRCAT(ZYDIS_MNEMONIC_, name), {__VA_ARGS__.rsvd = ENC_RW}, a, a}); }
	#define INSN_W_R_R(name, ...) \
		static minsn& name(mblock& blk, mreg a, mop b, mop c) { return blk.instructions.emplace_back(minsn{LI_STRCAT(ZYDIS_MNEMONIC_, name), {__VA_ARGS__.rsvd = ENC_W_R_R}, a, b, c}); }
	#define INSN_W_N_R_R(name, ...) \
		static minsn& name(mblock& blk, mreg a, mop b, mop c) { return blk.instructions.emplace_back(minsn{LI_STRCAT(ZYDIS_MNEMONIC_, name), {__VA_ARGS__.rsvd = ENC_W_N_R_R}, a, b, c}); }
	#define INSN_F_R_R(name, ...) \
		static minsn& name(mblock& blk, flag_id flag, mreg a, mop b) { return blk.instructions.emplace_back(minsn{LI_STRCAT(ZYDIS_MNEMONIC_, name), {__VA_ARGS__.rsvd = ENC_F_R_R}, flag, a, b}); }

	INSN_NOP(RDTSC, .implicit_gp_write = ((1u << (arch::from_native(zy::RAX) - 1)) | (1u << (arch::from_native(zy::RDX) - 1))), );
	INSN_RW(NEG);
	INSN_RW(NOT, .trashes_flags = false, );
	INSN_RW_R(SHR);
	INSN_RW_R(SHL);
	INSN_RW_R(ADD);
	INSN_RW_R(SUB);
	INSN_RW_R(OR);
	INSN_RW_R(AND);
	INSN_RW_R(IMUL);
	INSN_RW_R(XOR);
	INSN_RW_R(CMOVZ);
	INSN_RW_R(CMOVNBE);
	INSN_RW_R(CRC32); // Always qword.
	INSN_W_R(LEA, .trashes_flags = false, );
	INSN_W_R_R(BZHI, .trashes_flags = false, );
	INSN_W_R_R(RORX, .trashes_flags = false, );
	INSN_W_R_R(ROUNDSD, .trashes_flags = false, );
	INSN_W_N_R_R(VROUNDSD, .trashes_flags = false, );
	INSN_RW_R(DIVSD, .trashes_flags = false, );
	INSN_RW_R(MULSD, .trashes_flags = false, );
	INSN_RW_R(ADDSD, .trashes_flags = false, );
	INSN_RW_R(SQRTSD, .trashes_flags = false, );
	INSN_RW_R(SUBSD, .trashes_flags = false, );
	INSN_RW_R(ORPD, .trashes_flags = false, .force_size = 0x10, );
	INSN_RW_R(ANDPD, .trashes_flags = false, .force_size = 0x10, );
	INSN_RW_R(XORPD, .trashes_flags = false, .force_size = 0x10, );
	INSN_RW_R(MINSD, .trashes_flags = false, );
	INSN_RW_R(MAXSD, .trashes_flags = false, );
	INSN_W_R_R(VORPD, .trashes_flags = false, .force_size = 0x10, );
	INSN_W_R_R(VANDPD, .trashes_flags = false, .force_size = 0x10, );
	INSN_W_R_R(VXORPD, .trashes_flags = false, .force_size = 0x10, );
	INSN_W_R_R(VMINSD, .trashes_flags = false, );
	INSN_W_R_R(VMAXSD, .trashes_flags = false, );
	INSN_RW_R(PCMPEQB, .trashes_flags = false, );
	INSN_W_R_R(VPCMPEQB, .trashes_flags = false, );
	INSN_W_R_R(VDIVSD, .trashes_flags = false, );
	INSN_W_R_R(VMULSD, .trashes_flags = false, );
	INSN_W_R_R(VADDSD, .trashes_flags = false, );
	INSN_W_R_R(VSQRTSD, .trashes_flags = false, );
	INSN_W_R_R(VSUBSD, .trashes_flags = false, );
	INSN_F_R_R(CMP);
	INSN_F_R_R(TEST);
	INSN_F_R_R(PTEST);
	INSN_F_R_R(VPTEST);
	INSN_F_R_R(VUCOMISD);
	INSN_F_R_R(UCOMISD);

	// Operand helpers.
	//
	#define RI(x)    get_ri_for(b, x, false)
	#define RIi(x)   get_ri_for(b, x, true)
	#define RM(x)    get_rm_for(b, x)
	#define REG(x)   get_reg_for(b, x->as<insn>())
	#define REGV(x)   get_reg_for(b, x)
	#define YIELD(x) yield_value(b, i, x)

	#if !LI_DEBUG
		#define REF_VM() mop(intptr_t(b->source->L))
	#else
		#define REF_VM() mop(mreg(vreg_vm))
	#endif

	inline static int64_t extract_constant(value* v) {
		auto* c = v->as<constant>();
		if (c->vt == type::f32)
			return li::bit_cast<uint32_t>(float(c->n));
		else
			return c->i;
	}
	inline static mreg get_existing_reg(insn* i) {
		mreg r = li::bit_cast<mreg>((msize_t) i->visited);
		return r;
	}
	inline static mreg yield_value(mblock& b, insn* i, mop r) {
		if (auto dst = get_existing_reg(i)) {
			if (dst.is_fp())
				b.append(vop::movf, dst, r);
			else
				b.append(vop::movi, dst, r);
		} else {
			if (!r.is_reg()) {
				LI_ASSERT(r.is_const());
				if (i->vt == type::f64)
					b.append(vop::movf, (dst = b->next_fp()), r);
				else
					b.append(vop::movi, (dst = b->next_gp()), r);
			} else {
				dst = r.reg;
			}
			i->visited = li::bit_cast<msize_t>(dst);
		}
		return get_existing_reg(i);
	}
	inline static mreg get_reg_for(mblock& b, insn* i) {
		if (auto s = get_existing_reg(i)) {
			return s;
		} else if (i->vt == type::f32 || i->vt == type::f64) {
			return YIELD(b->next_fp());
		} else {
			return YIELD(b->next_gp());
		}
	}
	inline static mreg get_reg_for(mblock& b, value* i) {
		if (i->is<insn>()) {
			return get_reg_for(b, i->as<insn>());
		} else if (i->vt == type::f64 || i->vt == type::f32) {
			auto r = b->next_fp();
			b.append(vop::movf, r, extract_constant(i));
			return r;
		} else {
			auto r = b->next_gp();
			b.append(vop::movi, r, (int64_t) i->as<constant>()->i);
			return r;
		}
	}
	inline static mop get_ri_for(mblock& b, value* i, bool integer) {
		if (i->is<constant>()) {
			if (integer || i->vt == type::f32) {
				return mop(extract_constant(i));
			} else {
				return mop(i->as<constant>()->to_any());
			}
		} else {
			return get_reg_for(b, i->as<insn>());
		}
	}
	inline static mop get_rm_for(mblock& b, value* i) {
		if (i->is<constant>()) {
			if (auto c = i->as<constant>(); c->is(type::f64)) {
				auto dst = b->next_fp();
				if (c->i == 0) {
					if constexpr (USE_AVX) {
						VXORPD(b, dst, dst, dst);
						return dst;
					} else {
						XORPD(b, dst, dst);
						return dst;
					}
				} else if (c->i == -1ll) {
					if constexpr (USE_AVX) {
						VPCMPEQB(b, dst, dst, dst);
						return dst;
					} else {
						PCMPEQB(b, dst, dst);
						return dst;
					}
				}
			}
			return b->add_const(extract_constant(i));
		} else {
			return get_reg_for(b, i->as<insn>());
		}
	}

	// Emits a type check of the temporary given into a flag and sets the condition flag on the temporary.
	//
	inline static void check_type(mblock& b, value_type t, mreg out, mreg val) {
		LI_ASSERT(out != val);
		if (t == type_nil || t == type_exception) {
			RORX(b, out, val, 47);
			CMP(b, FLAG_Z, out, (int64_t) std::rotr(make_tag(t), 47));
			b.append(vop::setcc, out, FLAG_Z);
		} else if (t == type_number) {
			RORX(b, out, val, 47);
			AND(b, out, 0x1FFFF).target_info.force_size                                  = 4;
			CMP(b, FLAG_B, out, int64_t((make_tag(t) + 1) >> 47)).target_info.force_size = 4;
			b.append(vop::setcc, out, FLAG_B);
		} else {
			RORX(b, out, val, 47);
			AND(b, out, 0x1FFFF).target_info.force_size                            = 4;
			CMP(b, FLAG_Z, out, int64_t(make_tag(t) >> 47)).target_info.force_size = 4;
			b.append(vop::setcc, out, FLAG_Z);
		}
	}
	inline static void check_type_traitful(mblock& b, value_type t, mreg out, mreg val) {
		LI_ASSERT(out != val);
		constexpr uint64_t cmp = make_tag(type_gc_last_traitful + 1);
		b.append(vop::movi, out, (int64_t) cmp);
		CMP(b, FLAG_NBE, val, out);
		b.append(vop::setcc, out, FLAG_NBE);
	}
	inline static void check_type_gc(mblock& b, value_type t, mreg out, mreg val) {
		LI_ASSERT(out != val);
		constexpr uint64_t cmp = make_tag(type_gc_last + 1);
		b.append(vop::movi, out, (int64_t) cmp);
		CMP(b, FLAG_NBE, val, out);
		b.append(vop::setcc, out, FLAG_NBE);
	}

	// Clears type tag, should be a pointer type.
	//
	inline static void gc_type_clear(mblock& b, mreg dst, mreg src) {
	#if !LI_KERNEL_MODE
		auto tmp = b->next_gp();
		b.append(vop::movi, tmp, 47);
		BZHI(b, dst, src, tmp);
	#else
		if (dst != src) {
			b.append(vop::movi, dst, -1ll << 47);
			OR(b, dst, src);
		} else {
			auto tmp = b->next_gp();
			b.append(vop::movi, tmp, -1ll << 47);
			OR(b, dst, tmp);
		}
	#endif
	}

	// Erases the type into a general purpose register.
	//
	inline static void type_erase(mblock& b, mreg r, mreg out, type irty) {
		if (irty == type::nil) {
			b.append(vop::movi, out, nil);
		} else if (irty == type::exc) {
			b.append(vop::movi, out, exception_marker);
		} else if (irty == type::i1) {
			auto tmp = b->next_gp();
			// mov  tmp, tag
			b.append(vop::movi, tmp, (int64_t)mix_value(type_bool, 0));
			// movzx out, r
			b.append(vop::izx8, out, r);
			// or    out, tmp
			OR(b, out, tmp);
		} else if (irty == type::i8) {
			auto tg = b->next_gp();
			auto tf = b->next_fp();
			b.append(vop::isx8, tg, r);
			b.append(vop::fcvt, tf, tg);
			b.append(vop::movi, out, tf);
		} else if (irty == type::i16) {
			auto tg = b->next_gp();
			auto tf = b->next_fp();
			b.append(vop::isx16, tg, r);
			b.append(vop::fcvt, tf, tg);
			b.append(vop::movi, out, tf);
		} else if (irty == type::i32) {
			auto tg = b->next_gp();
			auto tf = b->next_fp();
			b.append(vop::isx32, tg, r);
			b.append(vop::fcvt, tf, tg);
			b.append(vop::movi, out, tf);
		} else if (irty == type::i64) {
			auto tf = b->next_fp();
			b.append(vop::fcvt, tf, r);
			b.append(vop::movi, out, tf);
		} else if (irty == type::f32) {
			auto tf = b->next_fp();
			b.append(vop::fx64, tf, r);
			b.append(vop::movi, out, tf);
		} else if (irty == type::f64 || irty == type::unk) {
			b.append(vop::movi, out, r);
		} else {
			auto ty = irty == type::opq ? type_opaque : type_table + (int(irty) - int(type::tbl));
			mreg fv;
	#if LI_KERNEL_MODE
			fv = b->next_gp();
			b.append(vop::movi, out, 47);
			BZHI(b, fv, r, out);
	#else
			if (out == r) {
				fv = b->next_gp();
				b.append(vop::movi, fv, r); // out and r are allowed to alias so we have to move beforehand.
			} else {
				fv = r;
			}
	#endif
			b.append(vop::movi, out, (int64_t) mix_value(uint8_t(ty), 0));
			OR(b, out, fv);
		}
	}
	inline static void type_erase(mblock& b, value* v, mreg out) {
		if (v->is<constant>()) {
			b.append(vop::movi, out, v->as<constant>()->to_any());
			return;
		} else if (v->vt == type::nil) {
			b.append(vop::movi, out, nil);
			return;
		} else if (v->vt == type::exc) {
			b.append(vop::movi, out, exception_marker);
			return;
		} else {
			type_erase(b, REG(v), out, v->vt);
		}
	}

	// Hashes the value as any::hash.
	//
	inline static void value_hash(mblock& b, mreg in, mreg out, value* v = nullptr) {
		// Skip if constant.
		//
		if (v && v->is<constant>()) {
			b.append(vop::movi, out, (int64_t) v->as<constant>()->to_any().hash());
			return;
		}

		// Replicate the hash function in-line.
		//
	#if LI_32 || !LI_HAS_CRC
		auto tmp2 = b->next_gp();
		b.append(vop::movi, out, 0xff51afd7ed558ccdll);
		b.append(vop::movi, tmp2, in);
		SHR(b, tmp2, 33);
		XOR(b, tmp2, in);
		IMUL(b, tmp2, out);
		b.append(vop::movi, out, tmp2);
		SHR(b, out, 33);
		XOR(b, out, tmp2);
	#else
		b.append(vop::movi, out, in);
		SHR(b, out, 8);
		CRC32(b, out, in);
	#endif
	}
};
#endif