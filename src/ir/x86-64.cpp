#include <util/common.hpp>
#if LI_JIT && LI_ARCH_X86 && !LI_32
#include <ir/mir.hpp>
#include <vm/runtime.hpp>
#include <vm/array.hpp>
#include <vm/table.hpp>

#if __AVX__
	static constexpr bool USE_AVX = true;
#else
	static constexpr bool USE_AVX = false;
#endif

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
	static constexpr flag_info flags[] = {
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

	// Operand helpers.
	//
	#define RI(x)    get_ri_for(b, x, false)
	#define RIi(x)    get_ri_for(b, x, true)
	#define RM(x)  get_rm_for(b, x)
	#define REG(x) get_reg_for(b, x->as<insn>())
	#define YIELD(x) yield_value(b, i, x)

	static mreg get_existing_reg(insn* i) {
		mreg r = std::bit_cast<mreg>((msize_t) i->visited);
		return r;
	}
	static mreg yield_value(mblock& b, insn* i, mop r) {
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
			i->visited = std::bit_cast<msize_t>(dst);
		}
		return get_existing_reg(i);
	}
	static mreg get_reg_for(mblock& b, insn* i) {
		if (auto s = get_existing_reg(i)) {
			return s;
		} else if (i->vt == type::f32 || i->vt == type::f64) {
			return YIELD(b->next_fp());
		} else {
			return YIELD(b->next_gp());
		}
	}
	static mop get_ri_for(mblock& b, value* i, bool integer) {
		if (i->is<constant>()) {
			if (integer) {
				auto c = i->as<constant>();
				LI_ASSERT(type::i8 <= c->vt && c->vt <= type::i64);
				return mop(c->i);
			} else {
				return mop(i->as<constant>()->to_any());
			}
		} else {
			return get_reg_for(b, i->as<insn>());
		}
	}
	static mop get_rm_for(mblock& b, value* i) {
		if (i->is<constant>()) {
			return b->add_const(i->as<constant>()->to_any());
		} else {
			return get_reg_for(b, i->as<insn>());
		}
	}

	// Instructions.
	//
	enum encoding_directive : msize_t {
		ENC_W_R,
		ENC_RW_R,
		ENC_RW,
		ENC_W_R_R,
		ENC_W_N_R_R,
		ENC_F_R_R,
	};

	#define INSN_W_R(name, ...) \
		static void name(mblock& blk, mreg a, mop b) { blk.instructions.push_back(minsn{LI_STRCAT(ZYDIS_MNEMONIC_, name), {__VA_ARGS__.rsvd = ENC_W_R}, a, b}); }
	#define INSN_RW_R(name, ...) \
		static void name(mblock& blk, mreg a, mop b) { blk.instructions.push_back(minsn{LI_STRCAT(ZYDIS_MNEMONIC_, name), {__VA_ARGS__ .rsvd = ENC_RW_R}, a, a, b}); }
	#define INSN_RW(name, ...) \
		static void name(mblock& blk, mreg a) { blk.instructions.push_back(minsn{LI_STRCAT(ZYDIS_MNEMONIC_, name), {__VA_ARGS__ .rsvd = ENC_RW}, a, a}); }
	#define INSN_W_R_R(name, ...) \
		static void name(mblock& blk, mreg a, mop b, mop c) { blk.instructions.push_back(minsn{LI_STRCAT(ZYDIS_MNEMONIC_, name), {__VA_ARGS__ .rsvd = ENC_W_R_R}, a, b, c}); }
	#define INSN_W_N_R_R(name, ...) \
		static void name(mblock& blk, mreg a, mop b, mop c) { blk.instructions.push_back(minsn{LI_STRCAT(ZYDIS_MNEMONIC_, name), {__VA_ARGS__ .rsvd = ENC_W_N_R_R}, a, b, c}); }
	#define INSN_F_R_R(name, ...) \
		static void name(mblock& blk, flag_id flag, mreg a, mop b) { blk.instructions.push_back(minsn{LI_STRCAT(ZYDIS_MNEMONIC_, name), {__VA_ARGS__ .rsvd=ENC_F_R_R}, flag, a, b}); }

	INSN_RW(NEG);
	INSN_RW(NOT, .trashes_flags = false, );
	INSN_RW_R(SHR);
	INSN_RW_R(SHL);
	INSN_RW_R(ADD);
	INSN_RW_R(SUB);
	INSN_RW_R(OR);
	INSN_RW_R(AND);
	INSN_RW_R(XOR);
	INSN_W_R_R(BZHI, .trashes_flags = false, );
	INSN_W_R_R(ROUNDSD, .trashes_flags = false, );
	INSN_W_N_R_R(VROUNDSD, .trashes_flags = false, );
	INSN_RW_R(DIVSD, .trashes_flags = false, );
	INSN_RW_R(MULSD, .trashes_flags = false, );
	INSN_RW_R(ADDSD, .trashes_flags = false, );
	INSN_RW_R(SUBSD, .trashes_flags = false, );
	INSN_RW_R(XORPS, .trashes_flags = false, );
	INSN_W_R_R(VXORPS, .trashes_flags = false, );
	INSN_W_R_R(VDIVSD, .trashes_flags = false, );
	INSN_W_R_R(VMULSD, .trashes_flags = false, );
	INSN_W_R_R(VADDSD, .trashes_flags = false, );
	INSN_W_R_R(VSUBSD, .trashes_flags = false, );
	INSN_F_R_R(CMP);
	INSN_F_R_R(TEST);
	INSN_F_R_R(PTEST);
	INSN_F_R_R(VPTEST);
	INSN_F_R_R(VUCOMISD);
	INSN_F_R_R(UCOMISD);

	// Emits a type check of the temporary given into a flag and sets the condition flag on the temporary.
	//
	static mreg check_type_cc(mblock& b, flag_id f, value_type t, mreg tmp) {
		uint64_t cmp;
		if (f == FLAG_Z && t == type_number) {
			f   = FLAG_B;
			cmp = (make_tag(uint8_t(t)) + 1) >> 47;
		} else {
			cmp = make_tag(uint8_t(t)) >> 47;
		}
		SHR(b, tmp, 47);
		CMP(b, f, tmp, cmp);
		b.append(vop::setcc, tmp, f);
		return tmp;
	}
	static mreg check_traitful(mblock& b, value_type t, mreg tmp) {
		uint64_t cmp = (make_tag(type_gc_last_traitful + 1) + 1);
		b.append(vop::movi, tmp, (int64_t)cmp);
		CMP(b, FLAG_NBE, tmp, cmp);
		b.append(vop::setcc, tmp, FLAG_NBE);
		return tmp;
	}

	// Clears type tag, should not be a number.
	//
	static void type_clear(mblock& b, mreg dst, mreg src) {
		auto tmp = b->next_gp();
		b.append(vop::movi, tmp, 47);
		BZHI(b, dst, src, tmp);
	}

	// Erases the type into a general purpose register.
	//
	static void type_erase(mblock& b, value* v, mreg out) {
		if (v->is<constant>()) {
			b.append(vop::movi, out, v->as<constant>()->to_any());
			return;
		} else if (v->vt == type::nil) {
			b.append(vop::movi, out, none);
			return;
		}

		auto r = REG(v);
		if (v->vt == type::i1) {
			auto tmp = b->next_gp();
			// mov tmp, false
			b.append(vop::movi, tmp, (int64_t) make_tag(type_false));
			// movzx out, r
			b.append(vop::izx8, out, r);
			// shl out, 47
			SHL(b, out, 47);
			// add out, tmp
			ADD(b, out, tmp);
		} else if (v->vt == type::i8) {
			auto tg = b->next_gp();
			auto tf = b->next_fp();
			b.append(vop::isx8, tg, r);
			b.append(vop::fcvt, tf, tg);
			b.append(vop::movi, out, tf);
		} else if (v->vt == type::i16) {
			auto tg = b->next_gp();
			auto tf = b->next_fp();
			b.append(vop::isx16, tg, r);
			b.append(vop::fcvt, tf, tg);
			b.append(vop::movi, out, tf);
		} else if (v->vt == type::i32) {
			auto tg = b->next_gp();
			auto tf = b->next_fp();
			b.append(vop::isx32, tg, r);
			b.append(vop::fcvt, tf, tg);
			b.append(vop::movi, out, tf);
		} else if (v->vt == type::i64) {
			auto tf = b->next_fp();
			b.append(vop::fcvt, tf, r);
			b.append(vop::movi, out, tf);
		} else if (v->vt == type::f32) {
			auto tf = b->next_fp();
			b.append(vop::fx64, tf, r);
			b.append(vop::movi, out, tf);
		} else if (v->vt == type::f64 || v->vt == type::unk) {
			b.append(vop::movi, out, r);
		} else {
			auto ty = v->vt == type::opq ? type_opaque : type_table + (int(v->vt) - int(type::tbl));
			mreg fv;
	#if LI_KERNEL_MODE
			fv = b->next_gp();
			b.append(vop::movi, out, 47);
			BZHI(b, fv, r, out);
	#else
			fv = r;
	#endif
			b.append(vop::movi, out, (int64_t) mix_value(uint8_t(ty), 0));
			OR(b, out, fv);
		}
	}

	// Emits floating-point comparison into a flag.
	//
	static flag_id fp_compare(mblock& b, operation cc, value* lhs, value* rhs) {
		LI_ASSERT(lhs->vt == type::f64);
		LI_ASSERT(rhs->vt == type::f64);

		// Can't have constant on LHS.
		//
		bool swapped = false;
		if (lhs->is<constant>()) {
			swapped = true;
			std::swap(lhs, rhs);
			LI_ASSERT(!lhs->is<constant>());
		}

		// If equality comparison and constant is zero:
		//
		auto lr  = REG(lhs);
		if (cc == bc::CNE || cc == bc::CEQ) {
			if (rhs->is<constant>() && rhs->as<constant>()->i == 0) {
				auto fl = cc == bc::CEQ ? FLAG_Z : FLAG_NZ;
				if constexpr (USE_AVX)
					VPTEST(b, fl, lr, lr);
				else
					PTEST(b, fl, lr, lr);
				return fl;
			}
		}

		// Map the operator.
		//
		auto    rh = RM(rhs);
		flag_id f;
		if (cc == bc::CLT) {
			f = FLAG_B;
		} else if (cc == bc::CGE) {
			f = FLAG_NB;
		} else if (cc == bc::CGT) {
			f = FLAG_NBE;
		} else {
			f = FLAG_BE;
		}
		if (swapped)
			f = flag_id(f ^ 1);

		// Emit the operation.
		//
		if constexpr (USE_AVX)
			VUCOMISD(b, f, lr, rh);
		else
			UCOMISD(b, f, lr, rh);
		return f;
	}

	// Emits floating-point unary/binary expression into a register.
	//
	static mreg fp_unary(mblock& b, operation op, value* rhs, insn* result = nullptr) {
		LI_ASSERT(rhs->vt == type::f64);

		// Get the registers.
		//
		mreg vx = result ? REG(result) : b->next_fp();
		mop  vr = REG(rhs);

		switch (op) {
			case bc::ANEG: {
				auto c = b->add_const(1ull << 63);
				if constexpr (USE_AVX) {
					VXORPS(b, vx, vr, c);
				} else {
					b.append(vop::movf, vx, vr);
					XORPS(b, vx, c);
				}
				return vx;
			}
			default:
				return {};
		}
	}
	static mreg fp_binary(mblock& b, operation op, value* lhs, value* rhs, insn* result = nullptr) {
		LI_ASSERT(lhs->vt == type::f64);
		LI_ASSERT(rhs->vt == type::f64);

		// Get the result register.
		//
		mreg vx = result ? REG(result) : b->next_fp();
		mreg vl;
		mop vr;

		// If LHS is a constant, try fixing it.
		//
		if (lhs->is<constant>()) {
			if (op == bc::AMUL || op == bc::AADD)
				std::swap(lhs, rhs);
		}

		// RHS can be a constant.
		//
		if (rhs->is<constant>()) {
			vr = b->add_const(any(rhs->as<constant>()->to_any().as_num()));
			vl = REG(lhs);
		}
		// If LHS is a constant, we need to load it into a temporary.
		//
		else if (lhs->is<constant>()) {
			auto tmp = b->next_fp();
			b.append(vop::movf, tmp, any(lhs->as<constant>()->to_any().as_num()));
			vl = tmp;
			vr = REG(rhs);
		}
		// Otherwise, if both are registers, all good.
		//
		else {
			vl = REG(lhs);
			vr = REG(rhs);
		}

		switch (op) {
			case bc::AADD: {
				if constexpr (USE_AVX) {
					VADDSD(b, vx, vl, vr);
				} else {
					b.append(vop::movf, vx, vl);
					ADDSD(b, vx, vr);
				}
				return vx;
			}
			case bc::ASUB: {
				if constexpr (USE_AVX) {
					VSUBSD(b, vx, vl, vr);
				} else {
					b.append(vop::movf, vx, vl);
					SUBSD(b, vx, vr);
				}
				return vx;
			}
			case bc::AMUL: {
				if constexpr (USE_AVX) {
					VMULSD(b, vx, vl, vr);
				} else {
					b.append(vop::movf, vx, vl);
					MULSD(b, vx, vr);
				}
				return vx;
			}
			case bc::ADIV: {
				if constexpr (USE_AVX) {
					VDIVSD(b, vx, vl, vr);
				} else {
					b.append(vop::movf, vx, vl);
					DIVSD(b, vx, vr);
				}
				return vx;
			}
			case bc::AMOD: {
				// TODO: AMOD as CCALL if not fast math.
				if constexpr (USE_AVX) {
					VDIVSD(b, vx, vl, vr);
					VROUNDSD(b, vx, vx, 11);  // = trunc(x/y)
					VMULSD(b, vx, vx, vr);
					VSUBSD(b, vx, vl, vx);
				} else {
					auto vt = b->next_fp();
					b.append(vop::movf, vx, vl);
					b.append(vop::movf, vt, vl);
					DIVSD(b, vt, vr);
					ROUNDSD(b, vt, vt, 11);
					MULSD(b, vt, vr);
					SUBSD(b, vx, vt);
				}
				return vx;
			}
			// TODO: APOW as CCALL
			default:
				return {};
		}
	}

	// Load/store from local.
	//
	static void local_load(mblock& b, mop idx, mreg out) {
		if (idx.is_const()) {
			int64_t disp = (idx.i64 + FRAME_SIZE + 1) * 8;
			int32_t disp32 = int32_t(disp);
			LI_ASSERT(disp32 == disp);

			if (out.is_fp())
				b.append(vop::loadf64, out, mmem{.base = vreg_args, .disp = disp32});
			else
				b.append(vop::loadi64, out, mmem{.base = vreg_args, .disp = disp32});
		} else if (idx.is_reg() && idx.reg.is_gp()) {
			if (out.is_fp())
				b.append(vop::loadf64, out, mmem{.base = vreg_args, .index = idx.reg, .scale = 8});
			else
				b.append(vop::loadi64, out, mmem{.base = vreg_args, .index = idx.reg, .scale = 8});
		} else {
			util::abort("invalid index value.");
		}
	}
	static void local_store(mblock& b, mop idx, mop in) {
		if (in.is_mem()) {
			auto r = b->next_gp();
			b.append(vop::loadi64, r, in);
			in     = r;
		} else if (in.is_const()) {
			auto r = b->next_gp();
			b.append(vop::movi, r, in);
			in = r;
		}

		if (idx.is_const()) {
			int64_t disp   = (idx.i64 + FRAME_SIZE + 1) * 8;
			int32_t disp32 = int32_t(disp);
			LI_ASSERT(disp32 == disp);

			if (in.reg.is_fp())
				b.append(vop::storef64, {}, mmem{.base = vreg_args, .disp = disp32}, in);
			else
				b.append(vop::storei64, {}, mmem{.base = vreg_args, .disp = disp32}, in);
		} else if (idx.is_reg() && idx.reg.is_gp()) {
			if (in.reg.is_fp())
				b.append(vop::storef64, {}, mmem{.base = vreg_args, .index = idx.reg, .scale = 8}, in);
			else
				b.append(vop::storei64, {}, mmem{.base = vreg_args, .index = idx.reg, .scale = 8}, in);
		} else {
			util::abort("invalid index value.");
		}
	}

	// Main lifter switch.
	//
	static void mlift(mblock& b, insn* i) {
		switch (i->opc) {
			// Locals.
			//
			case opcode::load_local: {
				local_load(b, RIi(i->operands[0]), REG(i));
				return;
			}
			case opcode::store_local: {
				local_store(b, RIi(i->operands[0]), REG(i->operands[1]));
				return;
			}

			// Complex types.
			//
			// TODO: None of this is right, just testing...
			//
			case opcode::field_set_raw: {
				// TODO: Inline if type known.
				//
				b.append(vop::movi, arch::map_gp_arg(0, 0), mreg(vreg_vm));
				type_erase(b, i->operands[0], arch::map_gp_arg(1, 0));
				type_erase(b, i->operands[1], arch::map_gp_arg(2, 0));
				type_erase(b, i->operands[2], arch::map_gp_arg(3, 0));
				b.append(vop::call, {}, (int64_t) &runtime::field_set_raw);
				// ^ result is potentially error!
				return;
			}
			case opcode::field_get:
			case opcode::field_get_raw: {
				b.append(vop::movi, arch::map_gp_arg(0, 0), mreg(vreg_vm));
				type_erase(b, i->operands[0], arch::map_gp_arg(1, 0));
				type_erase(b, i->operands[1], arch::map_gp_arg(2, 0));
				b.append(vop::call, {}, (int64_t) &runtime::field_get_raw);
				b.append(vop::movi, REG(i), mreg(arch::from_native(arch::gp_retval)));
				return;
			}
			case opcode::vlen: {
				// Pick the field.
				//
				int32_t offset;
				switch (i->operands[0]->vt) {
					case type::str:
						offset = offsetof(string, length);
						break;
					case type::tbl:
						offset = offsetof(table, active_count);
						break;
					case type::arr:
						offset = offsetof(array, length);
						break;
					default:
						util::abort("unexpected vlen with invalid or unknown type.");
				}

				// Load the result.
				//
				auto tg = b->next_gp();
				auto tf = b->next_fp();
				b.append(vop::loadi32, tg, mmem{.base = REG(i->operands[0]), .disp = offset});
				b.append(vop::izx32, tg, tg);
				b.append(vop::fcvt, tf, tg);
				b.append(vop::movi, REG(i), tf);
				return;
			}

			// Operators.
			//
			case opcode::unop: {
				LI_ASSERT(i->vt == type::f64);
				if (fp_unary(b, i->operands[0]->as<constant>()->vmopr, i->operands[1], i))
					return;
				break;
			}
			case opcode::binop: {
				LI_ASSERT(i->vt == type::f64);
				if (fp_binary(b, i->operands[0]->as<constant>()->vmopr, i->operands[1], i->operands[2], i))
					return;
				break;
			}

			// Upvalue.
			//
			case opcode::uval_get: {
				auto base = REG(i->operands[0]);
				auto idx  = RIi(i->operands[1]);
			
				// Add the upvalue offset and compute final offset.
				//
				mmem mem;
				if (idx.is_const()) {
					mem = {.base = base, .disp = int32_t(offsetof(function, upvalue_array) + idx.i64 * 8)};
				} else {
					mem = {.base = base, .index = idx.reg, .scale = 8, .disp = offsetof(function, upvalue_array)};
				}
			
				// Load the result.
				//
				b.append(vop::loadi64, REG(i), mem);
				return;
			}
			case opcode::uval_set: {
				auto base = REG(i->operands[0]);
				auto idx  = RIi(i->operands[1]);
				auto val  = b->next_gp();
				type_erase(b, i->operands[2], val);

				// Add the upvalue offset and compute final offset.
				//
				mmem mem;
				if (idx.is_const()) {
					mem = {.base = base, .disp = int32_t(offsetof(function, upvalue_array) + idx.i64 * 8)};
				} else {
					mem = {.base = base, .index = idx.reg, .scale = 8, .disp = offsetof(function, upvalue_array)};
				}
			
				// Write the result.
				//
				b.append(vop::storei64, {}, mem, val);
				return;
			}

			// Casts.
			//
			case opcode::assume_cast: {
				auto out = REG(i);
				switch (i->vt) {
					case type::i1: {
						auto tmp = b->next_gp();
						b.append(vop::movi, tmp, (int64_t) make_tag(type_true));
						CMP(b, FLAG_Z, tmp, REG(i->operands[0]));
						b.append(vop::setcc, out, FLAG_Z);
						return;
					}
					case type::i8:
					case type::i16:
					case type::i32:
					case type::i64: {
						b.append(vop::icvt, out, REG(i->operands[0]));
						return;
					}
					case type::f32:
					case type::f64: {
						b.append(vop::movf, out, REG(i->operands[0]));
						if (i->vt == type::f32)
							b.append(vop::fx32, out, out);
						return;
					}
					case type::opq: {
						b.append(vop::movi, out, REG(i->operands[0]));
						return;
					}
					// GC types.
					default: {
						type_clear(b, out, REG(i->operands[0]));
						return;
					}
					// Invalid types.
					//
					case type::nil:
					case type::unk: {
						break;
					}
				}
			}
			case opcode::coerce_cast: {
				LI_ASSERT(i->operands[1]->as<constant>()->irtype == type::i1);
				switch (i->operands[0]->vt) {
					case type::none:
					case type::nil: {
						b.append(vop::movi, REG(i), 0);
						return;
					}
					case type::unk: {
						static_assert(type_false == (type_none - 1), "Outdated constants.");

						auto tmp = REG(i);
						b.append(vop::movi, tmp, RIi(i->operands[0]));
						NOT(b, tmp);
						SHR(b, tmp, 47);
						SUB(b, tmp, (int64_t) type_false);
						CMP(b, FLAG_NBE, tmp, 1ll);
						b.append(vop::setcc, tmp, FLAG_NBE);
						return;
					}
					case type::i1: {
						b.append(vop::movi, REG(i), RIi(i->operands[0]));
						return;
					}
					default: {
						b.append(vop::movi, REG(i), 1);
						return;
					} 
				}
			}

			// Helpers used before transitioning to MIR.
			//
			case opcode::move: {
				YIELD(RI(i->operands[0]));
				return;
			}
			case opcode::erase_type: {
				type_erase(b, i->operands[0], REG(i));
				return;
			}

			// Conditionals.
			//
			case opcode::test_type: {
				auto vt = i->operands[1]->as<constant>()->vmtype;
				LI_ASSERT(i->operands[0]->vt == type::unk);

				auto tmp = REG(i);
				b.append(vop::movi, tmp, REG(i->operands[0]));
				check_type_cc(b, FLAG_Z, vt, tmp);
				return;
			}
			case opcode::test_traitful: {
				auto vt = i->operands[1]->as<constant>()->vmtype;
				LI_ASSERT(i->operands[0]->vt == type::unk);

				auto tmp = REG(i);
				b.append(vop::movi, tmp, REG(i->operands[0]));
				check_traitful(b, vt, tmp);
				return;
			}
			case opcode::compare: {
				auto cc   = i->operands[0]->as<constant>()->vmopr;
				auto flag = fp_compare(b, cc, i->operands[1], i->operands[2]);
				b.append(vop::setcc, REG(i), flag);
				return;
			}
			case opcode::select: {
				auto cc  = REG(i->operands[0]);
				auto lhs = REG(i->operands[1]);
				auto rhs = REG(i->operands[2]);
				b.append(vop::select, REG(i), cc, lhs, rhs);
				return;
			}
			case opcode::phi: {
				auto r = get_existing_reg(i->operands[0]->as<insn>());
				for (auto& op : i->operands) {
					LI_ASSERT(get_existing_reg(op->as<insn>()) == r);
				}
				YIELD(r);
				return;
			}

			// Call types.
			//
			case opcode::ccall: {
				// If it takes VM as it's first argument, add it.
				//
				int32_t gp_index = 0;
				int32_t fp_index = 0;
				if (i->operands[0]->as<constant>()->i1) {
					b.append(vop::movi, arch::map_gp_arg(0, 0), mreg(vreg_vm));
					gp_index++;
				}

				// Append each argument.
				//
				for (msize_t n = 3; n != i->operands.size(); n++) {
					auto& op = i->operands[n];

					if (op->vt == type::f32 || op->vt == type::f64) {
						auto r = arch::map_fp_arg(gp_index, fp_index++);
						if (r) {
							b.append(vop::movf, mreg(r), RI(op));
						} else {
							mmem mem{
								 .base = arch::from_native(arch::sp),
								 .disp = arch::stack_arg_begin + (gp_index + fp_index) * 8,
							};
							b.append(vop::storef64, {}, mem, REG(op));
							b->used_stack_length = std::max(b->used_stack_length, mem.disp + 8);
						}
					} else {
						auto r = arch::map_gp_arg(gp_index++, fp_index);
						if (r) {
							b.append(vop::movi, mreg(r), RIi(op));
						} else {
							mmem mem{
								 .base = arch::from_native(arch::sp),
								 .disp = arch::stack_arg_begin + (gp_index + fp_index) * 8,
							};
							b.append(vop::storei64, {}, mem, REG(op));
							b->used_stack_length = std::max(b->used_stack_length, mem.disp + 8);
						}
					}
				}
				// Write the call.
				//
				b.append(vop::call, {}, RIi(i->operands[2]));

				// Read the result.
				//
				if (auto ty = i->operands[1]->as<constant>()->irtype; (ty == type::f32 || ty == type::f64)) {
					b.append(vop::movf, REG(i), mreg(arch::from_native(arch::fp_retval)));
				} else if (ty != type::none) {
					b.append(vop::movi, REG(i), mreg(arch::from_native(arch::gp_retval)));
				}
				return;
			}

			// Block terminators.
			//
			case opcode::jcc: {
				b.append(vop::js, {}, REG(i->operands[0]), i->operands[1]->as<constant>()->bb->uid, i->operands[2]->as<constant>()->bb->uid);
				return;
			}
			case opcode::jmp: {
				b.append(vop::jmp, {}, i->operands[0]->as<constant>()->bb->uid);
				return;
			}

			// Procedure terminators.
			//
			case opcode::ret:
			case opcode::thrw: {
				auto code = i->opc == opcode::ret;
				auto tmp  = b->next_gp();
				type_erase(b, i->operands[0], tmp);
				local_store(b, FRAME_RET, tmp);
				b.append(vop::ret, {}, code ? 1ll : 0ll);
				return;
			}
			case opcode::unreachable: {
				b.append(vop::unreachable, {});
				return;
			}
			default:
				break;
		}
		util::abort("Opcode NYI: %s", i->to_string(true).c_str());
	}

	// Generates crude machine IR from the SSA IR.
	//
	std::unique_ptr<mprocedure> lift_ir(procedure* p) {
		auto m = std::make_unique<mprocedure>();
		m->source = p;

		// Clear all visitor state, we use both fields for mapping to machine structures.
		//
		m->source->clear_all_visitor_state();

		// Pre-allocate the block list and coalesce PHI nodes.
		//
		for (auto& b : m->source->basic_blocks) {
			auto* mb   = m->add_block();
			mb->hot = int32_t(b->loop_depth) - int32_t(b->cold_hint);

			b->visited = (uint64_t) mb;
			for (auto* phi : b->phis()) {
				// Allocate a register.
				//
				mreg r;
				if (phi->vt == type::f32 || phi->vt == type::f64)
					r = m->next_fp();
				else
					r = m->next_gp();

				// Force into all incoming blocks.
				//
				for (auto& op : phi->operands) {
					LI_ASSERT(op->is<insn>());

					auto* src = op->as<insn>();
					if (auto r2 = get_existing_reg(src)) {
						LI_ASSERT(r == r2);
					} else {
						src->visited = std::bit_cast<msize_t>(r);
					}
				}
			}
		}

		// For each block:
		//
		for (auto& b : m->source->basic_blocks) {
			//printf("-- Block $%u", b->uid);
			//if (b->cold_hint)
			//	printf(LI_CYN " [COLD %u]" LI_DEF, (uint32_t) b->cold_hint);
			//if (b->loop_depth)
			//	printf(LI_RED " [LOOP %u]" LI_DEF, (uint32_t) b->loop_depth);
			//putchar('\n');

			// Fix the jumps.
			//
			auto* mb = (mblock*) b->visited;
			for (auto& suc : b->successors)
				m->add_jump(mb, (mblock*) suc->visited);

			// Lift each instruction.
			//
			for (auto* i : b->insns()) {
				// printf(LI_GRN "#%-5x" LI_DEF "\t\t %s\n", i->source_bc, i->to_string(true).c_str());
				// size_t n = mb->instructions.size();
				mlift(*mb, i);
				// while (n != mb->instructions.size()) {
				//	puts(mb->instructions[n++].to_string().c_str());
				//}
			}
		}
		return m;
	}

	// Operand converters.
	//
	static constexpr int32_t MAGIC_RELOC_CPOOL  = 0x77777777;
	static constexpr int32_t MAGIC_RELOC_BRANCH = 0x77777778;
	static zy::reg to_reg(mblock& b, const mreg& r, size_t n = 0) {
		if (!r) {
			return zy::NO_REG;
		}

		zy::reg pr = arch::to_native(r.phys());
		if (n) {
			return zy::resize_reg(pr, n);
		} else {
			return pr;
		}
	}
	static zy::mem to_mem(mblock& b, const mmem& m) {
		if (m.base == vreg_cpool) {
			b->reloc_info.emplace_back(b->assembly.size(), -m.disp);
			return {
				 .size  = 8,
				 .base  = zy::RIP,
				 .index = to_reg(b, m.index),
				 .scale = (uint8_t) m.scale,
				 .disp  = MAGIC_RELOC_CPOOL,
			};
		} else {
			return {
				 .size  = 8,
				 .base  = to_reg(b, m.base),
				 .index = to_reg(b, m.index),
				 .scale = (uint8_t) m.scale,
				 .disp  = m.disp,
			};
		}
	}
	static ZydisEncoderOperand to_op(mblock& b, const mop& m) {
		if (m.is_const()) {
			return zy::to_encoder_op(m.i64);
		} else if (m.is_reg()) {
			return zy::to_encoder_op(to_reg(b, m.reg));
		} else if (m.is_mem()) {
			return zy::to_encoder_op(to_mem(b, m.mem));
		}
		util::abort("invalid operand");
	}

	static void assemble_native(mblock& b, const minsn& i) {
		ZydisEncoderRequest req;
		memset(&req, 0, sizeof(req));
		req.mnemonic      = arch::native_mnemonic(i.mnemonic);
		req.machine_mode  = ZYDIS_MACHINE_MODE_LONG_64;
		req.operand_count = 0;

		auto push_operand = [&](const mop& op) {
			//printf(" %s", op.to_string().c_str());
			req.operands[req.operand_count++] = to_op(b, op);
		};
		//printf("\t%s ", arch::name_mnemonic(req.mnemonic));
		switch (i.target_info.rsvd) {
			case ENC_W_R:
				push_operand(i.out);
				push_operand(i.arg[0]);
				break;
			case ENC_RW_R:
				push_operand(i.arg[0]);
				push_operand(i.arg[1]);
				break;
			case ENC_RW:
				push_operand(i.out);
				break;
			case ENC_W_R_R:
				push_operand(i.out);
				push_operand(i.arg[0]);
				push_operand(i.arg[1]);
				break;
			case ENC_W_N_R_R:
				push_operand(i.out);
				push_operand(i.out);
				push_operand(i.arg[0]);
				push_operand(i.arg[1]);
				break;
			case ENC_F_R_R:
				push_operand(i.arg[0]);
				push_operand(i.arg[1]);
				break;
			default:
				assume_unreachable();
				break;
		}
		LI_ASSERT(zy::encode(b->assembly, req));
	}
	static void assemble_virtual(mblock& b, const minsn& i) {
		switch (i.getv()) {
			case vop::movf: {
				auto  dst = to_reg(b, i.out);
				auto& src = i.arg[0];
				if (src.is_reg()) {
					if (src.reg.is_gp()) {
						constexpr auto mn = USE_AVX ? ZYDIS_MNEMONIC_VMOVQ : ZYDIS_MNEMONIC_MOVQ;
						LI_ASSERT(zy::encode(b->assembly, mn, dst, to_reg(b, src.reg)));
					} else if (src.reg != i.out) {
						constexpr auto mn = USE_AVX ? ZYDIS_MNEMONIC_VMOVAPD : ZYDIS_MNEMONIC_MOVAPD;
						LI_ASSERT(zy::encode(b->assembly, mn, dst, to_reg(b, src.reg)));
					}
				} else if (src.i64 == 0) {
					if constexpr (USE_AVX) {
						LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_VXORPS, dst, dst, dst));
					} else {
						LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_XORPS, dst, dst));
					}
				} else if (src.i64 == -1ll) {
					if constexpr (USE_AVX) {
						LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_VCMPSD, dst, dst, dst, 0));
					} else {
						LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_CMPSD, dst, dst, 0));
					}
				} else {
					auto           mem = b->add_const(i.arg[0].i64);
					constexpr auto mn  = USE_AVX ? ZYDIS_MNEMONIC_VMOVSD : ZYDIS_MNEMONIC_MOVSD;
					LI_ASSERT(zy::encode(b->assembly, mn, dst, to_op(b, mem)));
				}
				break;
			}
			case vop::movi: {
				auto dst = to_reg(b, i.out);
				auto& src = i.arg[0];
				if (src.is_reg()) {
					if (src.reg.is_gp()) {
						if (src.reg != i.out)
							LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOV, dst, to_reg(b, src.reg)));
					} else {
						constexpr auto mn = USE_AVX ? ZYDIS_MNEMONIC_VMOVQ : ZYDIS_MNEMONIC_MOVQ;
						LI_ASSERT(zy::encode(b->assembly, mn, dst, to_reg(b, src.reg)));
					}
				} else if (src.i64 == 0) {
					LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_XOR, dst, dst));
				} else if (src.i64 == int32_t(src.i64)) {
					LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOV, zy::resize_reg(dst, 4), src.i64));
				} else {
					LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOV, dst, src.i64));
				}
				break;
			}

			case vop::loadi8: {
				auto dst     = to_op(b, i.out);
				auto src     = to_op(b, i.arg[0]);
				src.mem.size = 1;
				LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOVZX, zy::resize_reg(dst.reg.value, 4), src));
				break;
			}
			case vop::loadi16: {
				auto dst     = to_op(b, i.out);
				auto src     = to_op(b, i.arg[0]);
				src.mem.size = 2;
				LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOVZX, zy::resize_reg(dst.reg.value, 4), src));
				break;
			}
			case vop::loadi32: {
				auto dst     = to_op(b, i.out);
				auto src     = to_op(b, i.arg[0]);
				src.mem.size = 4;
				LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOV, zy::resize_reg(dst.reg.value, 4), src));
				break;
			}
			case vop::loadi64: {
				auto dst = to_op(b, i.out);
				auto src = to_op(b, i.arg[0]);
				LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOV, dst, src));
				break;
			}
			case vop::loadf32: {
				auto dst          = to_op(b, i.out);
				auto src          = to_op(b, i.arg[0]);
				src.mem.size      = 4;
				constexpr auto mn  = USE_AVX ? ZYDIS_MNEMONIC_VMOVSS : ZYDIS_MNEMONIC_MOVSS;
				LI_ASSERT(zy::encode(b->assembly, mn, dst, src));
				break;
			}
			case vop::loadf64: {
				auto           dst = to_op(b, i.out);
				auto           src = to_op(b, i.arg[0]);
				constexpr auto mn  = USE_AVX ? ZYDIS_MNEMONIC_VMOVSD : ZYDIS_MNEMONIC_MOVSD;
				LI_ASSERT(zy::encode(b->assembly, mn, dst, src));
				break;
			}
			case vop::storei8: {
				auto dst     = to_op(b, i.arg[0]);
				auto src     = to_op(b, i.arg[1]);
				dst.mem.size = 1;
				LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOV, dst, zy::resize_reg(src.reg.value, 1)));
				break;
			}
			case vop::storei16: {
				auto dst     = to_op(b, i.arg[0]);
				auto src     = to_op(b, i.arg[1]);
				dst.mem.size = 2;
				LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOV, dst, zy::resize_reg(src.reg.value, 2)));
				break;
			}
			case vop::storei32: {
				auto dst     = to_op(b, i.arg[0]);
				auto src     = to_op(b, i.arg[1]);
				dst.mem.size = 4;
				LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOV, dst, zy::resize_reg(src.reg.value, 4)));
				break;
			}
			case vop::storei64: {
				auto dst = to_op(b, i.arg[0]);
				auto src = to_op(b, i.arg[1]);
				LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOV, dst, src));
				break;
			}
			case vop::storef32: {
				auto           dst = to_op(b, i.arg[0]);
				auto           src = to_op(b, i.arg[1]);
				dst.mem.size       = 4;
				constexpr auto mn  = USE_AVX ? ZYDIS_MNEMONIC_VMOVSS : ZYDIS_MNEMONIC_MOVSS;
				LI_ASSERT(zy::encode(b->assembly, mn, dst, src));
				break;
			}
			case vop::storef64: {
				auto           dst = to_op(b, i.arg[0]);
				auto           src = to_op(b, i.arg[1]);
				constexpr auto mn  = USE_AVX ? ZYDIS_MNEMONIC_VMOVSD : ZYDIS_MNEMONIC_MOVSD;
				LI_ASSERT(zy::encode(b->assembly, mn, dst, src));
				break;
			}
			case vop::setcc: {
				auto dst   = to_reg(b, i.out);
				auto dstb  = zy::resize_reg(dst, 1);
				auto setcc = flags[(msize_t) i.arg[0].reg.flag()].sets;
				LI_ASSERT(zy::encode(b->assembly, setcc, dstb));
				break;
			}
			case vop::select: {
				// Get the flag ID.
				//
				msize_t flag;
				if (i.arg[0].reg.is_flag()) {
					flag = (msize_t) i.arg[0].reg.flag();
				} else {
					auto r  = to_reg(b, i.arg[0].reg);
					auto rb = zy::resize_reg(r, 1);
					LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_TEST, rb, rb));
					flag = (msize_t) FLAG_NZ;
				}

				// Swap conditions if output is aliasing true.
				//
				auto o = to_reg(b, i.out);
				auto t = to_reg(b, i.arg[1].reg);
				auto f = to_reg(b, i.arg[2].reg);
				if (o == t) {
					flag ^= 1;
					std::swap(t, f);
				}

				if (i.out.is_gp()) {
					// ? mov  o, f
					//   cmov o, t
					if (o != f)
						LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOV, o, f));
					LI_ASSERT(zy::encode(b->assembly, flags[flag].cmovs, o, t));
				} else {
					util::abort("FP select NYI.");
				}
				break;
			}
			case vop::fx32: {
				auto dst = to_reg(b, i.out);
				auto src = to_reg(b, i.arg[0].reg);
				if constexpr (USE_AVX)
					LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_VCVTSD2SS, dst, dst, src));
				else
					LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_CVTSD2SS, dst, src));
				break;
			}
			case vop::fx64: {
				auto dst = to_reg(b, i.out);
				auto src = to_reg(b, i.arg[0].reg);
				if constexpr (USE_AVX)
					LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_VCVTSS2SD, dst, src));
				else
					LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_CVTSS2SD, dst, src));
				break;
			}
			case vop::fcvt: {
				auto dst = to_reg(b, i.out);
				auto src = to_reg(b, i.arg[0].reg);
				if constexpr (USE_AVX)
					LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_VCVTSI2SD, dst, dst, src));
				else
					LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_CVTSI2SD, dst, src));
				break;
			}
			case vop::icvt: {
				auto dst = to_reg(b, i.out);
				auto src = to_reg(b, i.arg[0].reg);
				if constexpr (USE_AVX)
					LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_VCVTTSD2SI, dst, src));
				else
					LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_CVTTSD2SI, dst, src));
				break;
			}
			case vop::izx8: {
				auto dst = zy::resize_reg(to_reg(b, i.out), 4);
				auto src = zy::resize_reg(to_reg(b, i.arg[0].reg), 1);
				LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOVZX, dst, src));
				break;
			}
			case vop::izx16: {
				auto dst = zy::resize_reg(to_reg(b, i.out), 4);
				auto src = zy::resize_reg(to_reg(b, i.arg[0].reg), 2);
				LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOVZX, dst, src));
				break;
			}
			case vop::izx32: {
				auto dst = zy::resize_reg(to_reg(b, i.out), 4);
				auto src = zy::resize_reg(to_reg(b, i.arg[0].reg), 4);
				LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOV, dst, src));
				break;
			}
			case vop::isx8: {
				auto dst = zy::resize_reg(to_reg(b, i.out), 4);
				auto src = zy::resize_reg(to_reg(b, i.arg[0].reg), 1);
				LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOVSX, dst, src));
				break;
			}
			case vop::isx16: {
				auto dst = zy::resize_reg(to_reg(b, i.out), 4);
				auto src = zy::resize_reg(to_reg(b, i.arg[0].reg), 2);
				LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOVSX, dst, src));
				break;
			}
			case vop::isx32: {
				auto dst = to_reg(b, i.out);
				auto src = zy::resize_reg(to_reg(b, i.arg[0].reg), 4);
				LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOVSXD, dst, src));
				break;
			}
			case vop::js: {
				// Get the flag ID.
				//
				msize_t flag;
				if (i.arg[0].reg.is_flag()) {
					flag = (msize_t) i.arg[0].reg.flag();
				} else {
					auto r  = to_reg(b, i.arg[0].reg);
					auto rb = zy::resize_reg(r, 1);
					LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_TEST, rb, rb));
					flag = (msize_t) FLAG_NZ;
				}

				// If true branch is closer to us, invert the condition.
				//
				arch::native_mnemonic mn           = flags[flag].js;
				auto                  true_branch  = i.arg[1].i64;
				auto                  false_branch = i.arg[2].i64;
				if (true_branch == (b.uid + 1) || (false_branch != (b.uid + 1) && true_branch < false_branch)) {
					std::swap(true_branch, false_branch);
					mn = flags[flag].jns;
				}

				// Emit the JCC.
				//
				b->reloc_info.emplace_back(b->assembly.size(), true_branch);
				LI_ASSERT(zy::encode(b->assembly, mn, MAGIC_RELOC_BRANCH));

				// Emit the next JMP.
				//
				if (false_branch != (b.uid + 1)) {
					b->reloc_info.emplace_back(b->assembly.size(), false_branch);
					LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_JMP, MAGIC_RELOC_BRANCH));
				}
				break;
			}
			case vop::jmp:
				if (i.arg[0].i64 != (b.uid + 1)) {
					b->reloc_info.emplace_back(b->assembly.size(), i.arg[0].i64);
					LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_JMP, MAGIC_RELOC_BRANCH));
				}
				break;
			case vop::ret: {
				b->assembly.insert(b->assembly.end(), b->epilogue.begin(), b->epilogue.end());
				if (i.arg[0].is_const()) {
					if (i.arg[0]) {
						LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOV, zy::RAX, i.arg[0].i64));
					} else {
						LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_XOR, zy::EAX, zy::EAX));
					}
					LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_RET));
				} else if (i.arg[0].is_reg()) {
					LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOV, zy::RAX, to_op(b, i.arg[0])));
					LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_RET));
				}
				break;
			}
			case vop::null:
				break;
			case vop::unreachable:
				LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_UD2));
				break;
			case vop::call:
				LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_MOV, zy::RAX, to_op(b, i.arg[0])));
				LI_ASSERT(zy::encode(b->assembly, ZYDIS_MNEMONIC_CALL, zy::RAX));
				break;
			default:
				util::abort("NYI");
				break;
		}
	}

	// Assembles the pseudo-target instructions in the IR.
	//
	jfunction* assemble_ir(mprocedure* proc) {
		// Sort the blocks by name.
		//
		proc->basic_blocks.sort([](mblock& a, mblock& b) { return a.uid < b.uid; });

		// Generate the epilogue and prologue.
		//
		{
			auto& prologue = proc->assembly;
			auto& epilogue = proc->epilogue;

			// Push non-vol GPs.
			//
			size_t push_count = 0;
			for (size_t i = std::size(arch::gp_volatile); i != arch::num_gp_reg; i++) {
				if ((proc->used_gp_mask >> i) & 1) {
					push_count++;
					auto reg = arch::gp_nonvolatile[i - std::size(arch::gp_volatile)];
					LI_ASSERT(zy::encode(prologue, ZYDIS_MNEMONIC_PUSH, reg));
				}
			}

			// Allocate space and align stack.
			//
			int  num_fp_used = std::popcount(proc->used_fp_mask >> std::size(arch::fp_volatile));
			auto alloc_bytes = num_fp_used * 0x10 + proc->used_stack_length - 8;
			alloc_bytes      = (alloc_bytes + 8) & ~7;
			LI_ASSERT(zy::encode(prologue, ZYDIS_MNEMONIC_SUB, arch::sp, alloc_bytes));

			// Save vector registers.
			//
			constexpr auto vector_move = USE_AVX ? ZYDIS_MNEMONIC_VMOVUPD : ZYDIS_MNEMONIC_MOVUPD;
			zy::mem vsave_it{.size = 0x10, .base = arch::sp, .disp = alloc_bytes};
			for (size_t i = std::size(arch::fp_volatile); i != arch::num_fp_reg; i++) {
				if ((proc->used_fp_mask >> i) & 1) {
					vsave_it.disp -= 0x10;
					LI_ASSERT(zy::encode(prologue, vector_move, vsave_it, arch::fp_nonvolatile[i - std::size(arch::fp_volatile)]));
				}
			}

			// Generate the opposite as epilogue.
			//
			vsave_it.disp = alloc_bytes;
			for (size_t i = std::size(arch::fp_volatile); i != arch::num_fp_reg; i++) {
				if ((proc->used_fp_mask >> i) & 1) {
					vsave_it.disp -= 0x10;
					constexpr auto vector_move = USE_AVX ? ZYDIS_MNEMONIC_VMOVUPD : ZYDIS_MNEMONIC_MOVUPD;
					LI_ASSERT(zy::encode(epilogue, vector_move, arch::fp_nonvolatile[i - std::size(arch::fp_volatile)], vsave_it));
				}
			}
			LI_ASSERT(zy::encode(epilogue, ZYDIS_MNEMONIC_ADD, arch::sp, alloc_bytes));
			for (size_t i = arch::num_gp_reg - 1; i >= std::size(arch::gp_volatile); i--) {
				if ((proc->used_gp_mask >> i) & 1) {
					auto reg = arch::gp_nonvolatile[i - std::size(arch::gp_volatile)];
					LI_ASSERT(zy::encode(epilogue, ZYDIS_MNEMONIC_POP, reg));
				}
			}
		}

		// Assemble all instructions.
		//
		for (auto& b : proc->basic_blocks) {
			b.asm_loc = proc->assembly.size();
			for (auto& i : b.instructions) {
				if (i.is_virtual) {
					assemble_virtual(b, i);
				} else {
					assemble_native(b, i);
				}
			}
		}

		// Allocate the code, cache-line align the assembly and append the constant pool.
		//
		vm*        L            = proc->source->L;
		size_t     asm_length   = (proc->assembly.size() + 63) & ~63;
		size_t     cpool_length = proc->const_pool.size() * sizeof(any);
		jfunction* out          = L->alloc<jfunction>(asm_length + cpool_length);
		memcpy(&out->code[0], proc->assembly.data(), proc->assembly.size());
		memset(&out->code[proc->assembly.size()], 0xCC, asm_length - proc->assembly.size());
		memcpy(&out->code[asm_length], proc->const_pool.data(), cpool_length);

		// Apply the relocations.
		//
		for (auto& [src, dst] : proc->reloc_info) {
			auto bytes = std::span<const uint8_t>(out->code, asm_length).subspan(src);

			auto input = bytes;
			auto dec   = zy::decode(input);
			LI_ASSERT(dec.has_value());

			auto* rip = bytes.data() + dec->ins.length;
			auto& rel = *(int32_t*) (bytes.data() + (dec->ins.raw.disp.offset ? dec->ins.raw.disp.offset : dec->ins.raw.imm->offset));

			void* target = nullptr;
			if (rel == MAGIC_RELOC_CPOOL) {
				target = out->code + asm_length - dst;
			} else if (rel == MAGIC_RELOC_BRANCH) {
				auto bb = range::find_if(proc->basic_blocks, [dst=dst](mblock& m) { return m.uid == dst; });
				LI_ASSERT(bb != proc->basic_blocks.end());
				target = out->code + bb->asm_loc;
			} else {
				util::abort("invalid reloc");
			}

			intptr_t disp = intptr_t(target) - intptr_t(rip);
			LI_ASSERT(int32_t(disp) == disp);
			rel = int32_t(disp);
		}

		//auto gen = std::span<const uint8_t>(out->code, asm_length);
		//while (auto i = zy::decode(gen)) {
		//	if (i->ins.mnemonic == ZYDIS_MNEMONIC_INT3)
		//		break;
		//	puts(i->to_string().c_str());
		//}
		return out;
	}
};

#endif