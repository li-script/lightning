#include <util/common.hpp>
#if LI_JIT && LI_ARCH_X86 && !LI_32
#include <mir/core.hpp>

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
	};
	#define ENUM_FLAGS(_) _(B,0) _(BE,2) _(L,4) _(LE,6) _(O,8) _(P,10) _(S,12) _(Z,14)
	static constexpr flag_info flags[] = {
	#define ENUMERATOR(F, id) \
	 flag_info{LI_STRCAT(ZYDIS_MNEMONIC_J, F), LI_STRCAT(ZYDIS_MNEMONIC_JN, F), LI_STRCAT(ZYDIS_MNEMONIC_SET, F), LI_STRCAT(ZYDIS_MNEMONIC_SETN, F)}, \
	 flag_info{LI_STRCAT(ZYDIS_MNEMONIC_JN, F), LI_STRCAT(ZYDIS_MNEMONIC_J, F), LI_STRCAT(ZYDIS_MNEMONIC_SETN, F), LI_STRCAT(ZYDIS_MNEMONIC_SET, F)},
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
		mreg r = std::bit_cast<mreg>((uint32_t)i->visited);
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
			i->visited = std::bit_cast<uint32_t>(dst);
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
	#define INSN_W_R(name)                                                           \
		static size_t name(mblock& blk, mreg a, mop b) {                              \
			size_t n = blk.instructions.size();                                        \
			blk.instructions.push_back(minsn{LI_STRCAT(ZYDIS_MNEMONIC_, name), a, b}); \
			return n;                                                                  \
		}
	#define INSN_RW_R(name)																				 \
	static size_t name(mblock& blk, mreg a, mop b) {											 \
		size_t n = blk.instructions.size();															 \
		blk.instructions.push_back(minsn{LI_STRCAT(ZYDIS_MNEMONIC_, name), a, a, b});	 \
		return n;																							 \
	}
	#define INSN_RW(name)                                                            \
		static size_t name(mblock& blk, mreg a) {                                     \
			size_t n = blk.instructions.size();                                        \
			blk.instructions.push_back(minsn{LI_STRCAT(ZYDIS_MNEMONIC_, name), a, a}); \
			return n;                                                                  \
		}
	#define INSN_W_R_R(name)                                                            \
		static size_t name(mblock& blk, mreg a, mop b, mop c) {                          \
			size_t n = blk.instructions.size();                                           \
			blk.instructions.push_back(minsn{LI_STRCAT(ZYDIS_MNEMONIC_, name), a, b, c}); \
			return n;                                                                     \
		}
	#define INSN_F_R_R(name)                                                                        \
		static size_t name(mblock& blk, flag_id flag, mreg a, mop b) {                               \
			size_t n = blk.instructions.size();                                                       \
			blk.instructions.push_back(minsn{LI_STRCAT(ZYDIS_MNEMONIC_, name), flag, a, b}); \
			return n;                                                                                 \
		}
	INSN_RW(NEG);
	INSN_RW(NOT);
	INSN_RW_R(SHR);
	INSN_RW_R(SHL);
	INSN_RW_R(ADD);
	INSN_RW_R(SUB);
	INSN_RW_R(OR);
	INSN_RW_R(AND);
	INSN_RW_R(XOR);
	INSN_W_R_R(SHLX);
	INSN_W_R_R(SHRX);
	INSN_W_R_R(BZHI);

	INSN_W_R(CVTSI2SD);
	INSN_W_R(CVTSS2SD);
	INSN_W_R_R(ROUNDSD);
	INSN_W_R(VCVTSI2SD);  // TODO: Need three operand form for encoding!
	INSN_W_R(VCVTSS2SD);  // TODO: Need three operand form for encoding!
	INSN_W_R_R(VROUNDSD);  // TODO: Need four operand form for encoding!

	INSN_W_R(DIVSD);
	INSN_W_R(MULSD);
	INSN_W_R(ADDSD);
	INSN_W_R(SUBSD);
	INSN_W_R(XORPS);
	INSN_W_R_R(VXORPS);
	INSN_W_R_R(VDIVSD);
	INSN_W_R_R(VMULSD);
	INSN_W_R_R(VADDSD);
	INSN_W_R_R(VSUBSD);

	INSN_F_R_R(CMP);
	INSN_F_R_R(TEST);
	INSN_F_R_R(VPTEST);
	INSN_F_R_R(PTEST);
	INSN_F_R_R(VUCOMISD);
	INSN_F_R_R(UCOMISD);

	// Emits a type check of the temporary given into a flag and sets the condition flag on the temporary.
	//
	static mreg check_type_cc(mblock& b, flag_id f, value_type t, mreg tmp) {
		if (f == FLAG_Z && t == type_number) {
			f = FLAG_B;
		}
		uint64_t cmp = (make_tag(uint8_t(t))+1) >> 47;
		SHR(b, tmp, 47);
		CMP(b, f, tmp, cmp);
		b.append(vop::setcc, tmp, f);
		return tmp;
	}

	// Erases the type into a general purpose register.
	//
	static mreg type_erase(mblock& b, value* v) {
		if (v->is<constant>()) {
			auto r = b->next_gp();
			b.append(vop::movi, r, v->as<constant>()->to_any());
			return r;
		} else if (v->vt == type::nil) {
			auto r = b->next_gp();
			b.append(vop::movi, r, none);
			return r;
		}

		auto r = REG(v);
		if (v->vt == type::i1) {
			auto tmp1 = b->next_gp();
			auto tmp2 = b->next_gp();
			// mov R1, 47
			b.append(vop::movi, tmp1, 47);
			// shlx R1, R0, R1
			SHLX(b, tmp1, r, tmp1);
			// mov R2, false
			b.append(vop::movi, tmp2, (int64_t)make_tag(type_false));
			// mov R2, R1
			ADD(b, tmp2, tmp1);
			return tmp2;
		} else if (type::i8 <= v->vt && v->vt <= type::i64) {
			auto tf = b->next_fp();
			auto tg = b->next_gp();
			if constexpr (USE_AVX)
				VCVTSI2SD(b, tf, r);
			else
				CVTSI2SD(b, tf, r);
			b.append(vop::movi, tg, tf);
			return tg;
		} else if (v->vt == type::f32) {
			auto tf = b->next_fp();
			auto tg = b->next_gp();
			if constexpr (USE_AVX)
				VCVTSS2SD(b, tf, r);
			else
				CVTSS2SD(b, tf, r);
			b.append(vop::movi, tg, tf);
			return tg;
		} else if (v->vt == type::f64) {
			auto tg = b->next_gp();
			b.append(vop::movi, tg, r);
			return tg;
		} else if (v->vt == type::unk) {
			return r;
		} else {
			auto ty = v->vt == type::opq ? type_opaque : type_table + (int(v->vt) - int(type::tbl));
			auto t1 = b->next_gp();
			auto t2 = b->next_gp();
			b.append(vop::movi, t1, 47);
			BZHI(b, t2, r, t1);
			b.append(vop::movi, t1, (int64_t)mix_value(uint8_t(ty), 0));
			OR(b, t1, t2);
			return t1;
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
			int64_t disp = idx.i64 * 8;
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
			int64_t disp   = idx.i64 * 8;
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
	static string* mlift(mblock& b, insn* i) {
		switch (i->opc) {
			case opcode::load_local: {
				local_load(b, RIi(i->operands[0]), REG(i));
				return nullptr;
			}
			case opcode::store_local: {
				local_store(b, RIi(i->operands[0]), REG(i->operands[1]));
				return nullptr;
			}
			case opcode::coerce_cast: {
				LI_ASSERT(i->operands[1]->as<constant>()->irtype == type::i1);
				switch (i->operands[0]->vt) {
					case type::none:
					case type::nil: {
						b.append(vop::movi, REG(i), 0);
						return nullptr;
					}
					case type::unk: {
						static_assert(type_false == (type_none - 1), "update logic.");

						auto tmp = REG(i);
						b.append(vop::movi, tmp, RIi(i->operands[0]));
						NOT(b, tmp);
						SHR(b, tmp, 47);
						SUB(b, tmp, (int64_t) type_false);
						CMP(b, FLAG_NBE, tmp, 1ll);
						b.append(vop::setcc, tmp, FLAG_NBE);
						return nullptr;
					}
					case type::i1: {
						b.append(vop::movi, REG(i), RIi(i->operands[0]));
						return nullptr;
					}
					default: {
						b.append(vop::movi, REG(i), 1);
						return nullptr;
					} 
				}
			}
			case opcode::test_type: {
				auto vt = i->operands[1]->as<constant>()->vmtype;
				LI_ASSERT(i->operands[0]->vt == type::unk);

				auto tmp = REG(i);
				b.append(vop::movi, tmp, REG(i->operands[0]));
				check_type_cc(b, FLAG_Z, vt, tmp);
				return nullptr;
			}
			case opcode::jcc: {
				b.append(vop::js, {}, REG(i->operands[0]), i->operands[1]->as<constant>()->bb->uid, i->operands[2]->as<constant>()->bb->uid);
				return nullptr;
			}
			case opcode::jmp: {
				b.append(vop::jmp, {}, i->operands[0]->as<constant>()->bb->uid);
				return nullptr;
			}
			case opcode::assume_cast: {
				auto out = REG(i);
				b.append(out.is_fp() ? vop::movf : vop::movi, out, REG(i->operands[0]));
				return nullptr;
			}
			case opcode::compare: {
				auto cc = i->operands[0]->as<constant>()->vmopr;
				auto flag = fp_compare(b, cc, i->operands[1], i->operands[2]);
				b.append(vop::setcc, REG(i), flag);
				return nullptr;
			}
			case opcode::erase_type: {
				YIELD(type_erase(b, i->operands[0]));
				return nullptr;
			}
			case opcode::move: {
				YIELD(RI(i->operands[0]));
				return nullptr;
			}
			case opcode::unop: {
				if (i->vt == type::f64) {
					if (fp_unary(b, i->operands[0]->as<constant>()->vmopr, i->operands[1], i))
						return nullptr;
				}
				break;
			}
			case opcode::binop: {
				if (i->vt == type::f64) {
					if (fp_binary(b, i->operands[0]->as<constant>()->vmopr, i->operands[1], i->operands[2], i))
						return nullptr;
				}
				break;
			}
			case opcode::unreachable: {
				b.append(vop::unreachable, {});
				return nullptr;
			}
			case opcode::phi: {
				auto r = get_existing_reg(i->operands[0]->as<insn>());
				for (auto& op : i->operands) {
					LI_ASSERT(get_existing_reg(op->as<insn>()) == r);
				}
				YIELD(r);
				return nullptr;
			}
			case opcode::thrw: 
			case opcode::ret: {
				auto code = i->opc == opcode::ret;
				local_store(b, FRAME_RET, RI(i->operands[0]));
				b.append(vop::ret, {}, code ? 1ll : 0ll);
				return nullptr;
			}
			default:
				break;
		}
		return b->error("Opcode %s NYI", i->to_string(true).c_str());
	}


	string* lift_to_mir(mprocedure* m) {
		// Clear all visitor state, we use both fields for mapping to machine structures.
		//
		m->source->clear_all_visitor_state();

		// Pre-allocate the block list and coalesce PHI nodes.
		//
		for (auto& b : m->source->basic_blocks) {
			b->visited = (uint64_t) m->add_block();

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
						src->visited = std::bit_cast<uint32_t>(r);
					}
				}
			}
		}

		// For each block:
		//
		for (auto& b : m->source->basic_blocks) {
			printf("-- Block $%u", b->uid);
			if (b->cold_hint)
				printf(LI_CYN " [COLD %u]" LI_DEF, (uint32_t) b->cold_hint);
			if (b->loop_depth)
				printf(LI_RED " [LOOP %u]" LI_DEF, (uint32_t) b->loop_depth);
			putchar('\n');

			// Fix the jumps.
			//
			auto* mb = (mblock*) b->visited;
			for (auto& suc : b->successors)
				m->add_jump(mb, (mblock*) suc->visited);

			// Lift each instruction.
			//
			for (auto* i : b->insns()) {
				//printf(LI_GRN "#%-5x" LI_DEF " %s\n", i->source_bc, i->to_string(true).c_str());

				size_t n = mb->instructions.size();
				string* err = mlift(*mb, i);
				while (n != mb->instructions.size()) {
					puts(mb->instructions[n++].to_string().c_str());
				}
				if (err)
					return err;
			}
		}

		return m->error("lolz");
	}
	string* mir_assemble(mprocedure* m);
};

#endif