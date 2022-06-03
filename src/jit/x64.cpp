#include <util/common.hpp>
#include <jit/target.hpp>
#if LI_JIT && LI_ARCH_X86 && !LI_32

#include <ir/arch.hpp>
#include <ir/value.hpp>
#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/lifter.hpp>
#include <vm/function.hpp>
#include <jit/regalloc.hpp>
#include <jit/zydis.hpp>

namespace li::ir::jit {
	// Wrapper to handle automatic reg->ZydisReg conversion.
	//
	template<typename T>
	static constexpr auto operand_launder(const T& op) {
		if constexpr (std::is_same_v<T, arch::reg>) {
			LI_ASSERT(op != 0);
			return arch::to_native(op);
		} else {
			return op;
		}
	}

	static constexpr ZydisMnemonic jcc_reverse(ZydisMnemonic m) {
		switch ( m ) {
			 case ZYDIS_MNEMONIC_JB:
				 return ZYDIS_MNEMONIC_JNB;
			 case ZYDIS_MNEMONIC_JNB:
				 return ZYDIS_MNEMONIC_JB;
			 case ZYDIS_MNEMONIC_JBE:
				 return ZYDIS_MNEMONIC_JNBE;
			 case ZYDIS_MNEMONIC_JNBE:
				 return ZYDIS_MNEMONIC_JBE;
			 case ZYDIS_MNEMONIC_JLE:
				 return ZYDIS_MNEMONIC_JNLE;
			 case ZYDIS_MNEMONIC_JNLE:
				 return ZYDIS_MNEMONIC_JLE;
			 case ZYDIS_MNEMONIC_JL:
				 return ZYDIS_MNEMONIC_JNL;
			 case ZYDIS_MNEMONIC_JNL:
				 return ZYDIS_MNEMONIC_JL;
			 case ZYDIS_MNEMONIC_JO:
				 return ZYDIS_MNEMONIC_JNO;
			 case ZYDIS_MNEMONIC_JNO:
				 return ZYDIS_MNEMONIC_JO;
			 case ZYDIS_MNEMONIC_JP:
				 return ZYDIS_MNEMONIC_JNP;
			 case ZYDIS_MNEMONIC_JNP:
				 return ZYDIS_MNEMONIC_JP;
			 case ZYDIS_MNEMONIC_JS:
				 return ZYDIS_MNEMONIC_JNS;
			 case ZYDIS_MNEMONIC_JNS:
				 return ZYDIS_MNEMONIC_JS;
			 case ZYDIS_MNEMONIC_JZ:
				 return ZYDIS_MNEMONIC_JNZ;
			 case ZYDIS_MNEMONIC_JNZ:
				 return ZYDIS_MNEMONIC_JZ;
			 default:
				 util::abort("invalid cc");
		}
	}


	static constexpr int32_t magic_reloc = 0xAABBC2;

	// Machine-code block.
	//
	struct mc_procedure;
	struct mc_block {
		// Parent.
		//
		mc_procedure* mproc = nullptr;

		// Mapping to IR.
		//
		basic_block* ibb = nullptr;

		// Block label.
		//
		uint32_t label = 0;

		// Raw bytes emitted.
		//
		std::vector<uint8_t> code;

		// Bytecode position mapping.
		//
		std::vector<std::pair<bc::pos, size_t>> bc_to_ip = {};

		// Relocation requests.
		// - [offset, block id]
		//
		std::vector<std::pair<size_t, uint32_t>> relocs;

		// Data relocations for constants.
		// - [offset of insn start (not rel!), data id]
		//
		std::vector<uint64_t>                  data;
		std::vector<std::pair<size_t, size_t>> data_relocs;

		// Pending epilogue/prologue flags.
		//
		uint8_t pending_prologue : 1 = 0;
		uint8_t pending_epilogue : 1 = 0;

		// Hotness.
		//
		int32_t hot = 0;

		// Adds an instruction.
		//
		template<typename... Tx>
		void emit(ZydisMnemonic mnemonic, const Tx&... operands) {
			LI_ASSERT(zy::encode(code, mnemonic, operand_launder(operands)...));
		}
		template<typename... Tx>
		void operator()(ZydisMnemonic mnemonic, const Tx&... operands) {
			emit(mnemonic, operands...);
		}

		// Moves an uint64_t/double into the constant list and returns a memory operand.
		//
		zy::mem ref_const(uint64_t value) {
			data_relocs.emplace_back(code.size(), data.size());
			data.emplace_back(value);
			return zy::mem{.size = 8, .base = zy::RIP, .disp = magic_reloc};
		}
		zy::mem ref_const(double value) { return ref_const(bit_cast<uint64_t>(value)); }

		// Move helper.
		//
		void move(zy::mem dst, arch::reg src) {
			if (arch::is_gp(src)) {
				emit(ZYDIS_MNEMONIC_MOV, dst, src);
			} else {
	#if __AVX__
				emit(ZYDIS_MNEMONIC_VMOVQ, dst, src);
	#else
				emit(ZYDIS_MNEMONIC_MOVSD, dst, src);
	#endif
			}
		}
		void move(arch::reg dst, zy::mem src) {
			if (arch::is_gp(dst)) {
				emit(ZYDIS_MNEMONIC_MOV, dst, src);
			} else {
	#if __AVX__
				emit(ZYDIS_MNEMONIC_VMOVQ, dst, src);
	#else
				emit(ZYDIS_MNEMONIC_MOVSD, dst, src);
	#endif
			}
		}
		void move(arch::reg dst, arch::reg src) {
			if (src == dst)
				return;

			if (arch::is_gp(dst)) {
				if (arch::is_gp(src)) {
					emit(ZYDIS_MNEMONIC_MOV, dst, src);
				} else {
	#if __AVX__
					emit(ZYDIS_MNEMONIC_VMOVQ, dst, src);
	#else
					emit(ZYDIS_MNEMONIC_MOVQ, dst, src);
	#endif
				}
			} else {
				if (arch::is_gp(src)) {
	#if __AVX__
					emit(ZYDIS_MNEMONIC_VMOVQ, dst, src);
	#else
					emit(ZYDIS_MNEMONIC_MOVQ, dst, src);
	#endif
				} else {
	#if __AVX__
					emit(ZYDIS_MNEMONIC_VMOVQ, dst, src);
	#else
					emit(ZYDIS_MNEMONIC_MOVSD, dst, src);
	#endif
				}
			}
		}

		// Member reference helper.
		//
		template<typename F, typename C>
		zy::mem ref(ZydisRegister base, F C::*field) {
			return {.size = sizeof(F), .base = base, .disp = (intptr_t) (&(((C*) nullptr)->*field))};
		}
	};

	// Machine-code procedure.
	//
	struct mc_procedure {
		// Blocks.
		//
		std::list<mc_block> blocks = {};

		// Next non-IR block label.
		//
		uint32_t next_label = 0;

		// Apppends a block mapping to an IR basic block.
		//
		mc_block& add_block(basic_block* bb) {
			auto& blk = blocks.emplace_back();
			blk.mproc = this;
			blk.ibb   = bb;
			blk.label = bb ? bb->uid : next_label++;
			return blk;
		}
	};


	// Lowers a single instruction, returns non-nullptr on error.
	//
	static string* lower(mc_block& mc, reg_allocator& reg, insn* i, uint32_t ip) {
		switch (i->opc) {
			case opcode::load_local: {
				auto out_reg = reg.get_anyreg(ip, i->name, reg.ideal_reg_type(i->name), true);
				mc.move(out_reg, zy::mem{.size = 8, .base = arch::bp, .disp = i->operands[0]->as<constant>()->i32*8});
				return nullptr;
			}
			case opcode::store_local: {
				auto rhs    = i->operands[1]->as<insn>();
				auto in_reg = reg.get_anyreg(ip, rhs->name, reg.ideal_reg_type(rhs->name));
				mc.move(zy::mem{.size = 8, .base = arch::bp, .disp = i->operands[0]->as<constant>()->i32 * 8}, in_reg);
				return nullptr;
			}
			case opcode::phi: {
				// Make sure all operands are in the same instruction.
				//
				uint32_t input = 0;
				for (auto& op : i->operands) {
					LI_ASSERT(op->is<insn>());
					if (input == 0) {
						input = op->as<insn>()->name;
					} else {
						auto r2 = op->as<insn>()->name;
						LI_ASSERT(r2 == input);
						input = r2;
					}
				}
				LI_ASSERT(input != 0);

				// If our output is not mapped, emit a simple move.
				//
				if (i->name != input) {
					auto in_reg = reg.get_anyreg(ip, input, reg.ideal_reg_type(input));
					auto out_reg = reg.get_anyreg(ip, i->name, arch::is_gp(in_reg), true);
					if (!out_reg)
						return string::create(mc.ibb->proc->L, "register allocation failed.");
					mc.move(out_reg, in_reg);
				}
				return nullptr;
			}
			case opcode::compare: {
				auto cc  = i->operands[0]->as<constant>()->vmopr;
				auto lhs = i->operands[1];
				auto rhs = i->operands[2];

				auto to_reg = [&](value* v, std::optional<bool> ty) {
					if (v->is<insn>()) {
						auto i = v->as<insn>();
						return reg.get_anyreg(ip, i->name, ty ? *ty : reg.ideal_reg_type(i->name));
					} else {
						return arch::reg_none;
					}
				};
				auto load_const = [&](constant* v) {
					auto r = reg.alloc_next(ip, true);
					if (r) {
						mc(ZYDIS_MNEMONIC_MOV, r, v->to_any().value);
					}
					return r;
				};

				// Equality comparison, don't care about the type.
				//
				if (cc == bc::CNE || cc == bc::CEQ) {
					auto test_zero = [&](value* v) -> string* {
						auto reg = to_reg(v, std::nullopt);
						if (!reg)
							return string::create(mc.ibb->proc->L, "register allocation failed.");

						if (arch::is_gp(reg)) {
							mc(ZYDIS_MNEMONIC_TEST, reg, reg);
						} else {
	#if __AVX__
							mc(ZYDIS_MNEMONIC_VPTEST, reg, reg);
	#else
							mc(ZYDIS_MNEMONIC_PTEST, reg, reg);
	#endif
						}
						i->visited = cc == bc::CNE ? ZYDIS_MNEMONIC_JNZ : ZYDIS_MNEMONIC_JZ;
						return nullptr;
					};

					// Test zero.
					//
					if (lhs->is<constant>() && lhs->as<constant>()->i == 0) {
						return test_zero(rhs);
					} else if (rhs->is<constant>() && rhs->as<constant>()->i == 0) {
						return test_zero(lhs);
					}

					// If any of the values is a constant, try to get GP, otherwise try to match.
					//
					arch::reg lhs_r = arch::reg_none, rhs_r = arch::reg_none;
					if (lhs->is<constant>()) {
						rhs_r = to_reg(rhs, true);
						lhs_r = load_const(lhs->as<constant>());
					} else if (rhs->is<constant>()) {
						lhs_r = to_reg(lhs, true);
						rhs_r = load_const(rhs->as<constant>());
					} else {
						auto lhs_i = lhs->as<insn>();
						auto rhs_i = rhs->as<insn>();
						lhs_r = reg.check_reg(lhs_i->name);
						if (!lhs_r) {
							rhs_r = to_reg(rhs, std::nullopt);
							lhs_r = to_reg(lhs, arch::is_gp(rhs_r));
						} else {
							rhs_r = to_reg(rhs, arch::is_gp(lhs_r));
						}
					}

					// Assert register allocation.
					//
					if (!lhs_r || !rhs_r)
						return string::create(mc.ibb->proc->L, "register allocation failed.");

					// If GP, emit CMP.
					//
					if (arch::is_gp(lhs_r)) {
						mc(ZYDIS_MNEMONIC_CMP, lhs_r, rhs_r);
					}
					// If FP, emit VPXOR + VPTEST.
					//
					else {
						auto tmp = reg.alloc_next(ip, false);
						if (!tmp)
							return string::create(mc.ibb->proc->L, "register allocation failed.");
	#if __AVX__
						mc(ZYDIS_MNEMONIC_VPXOR, tmp, lhs_r, rhs_r);
						mc(ZYDIS_MNEMONIC_VPTEST, tmp, tmp);
	#else
						mc(ZYDIS_MNEMONIC_MOVSD, tmp, lhs_r);
						mc(ZYDIS_MNEMONIC_PXOR, tmp, rhs_r);
						mc(ZYDIS_MNEMONIC_PTEST, tmp, reg);
	#endif
					}
					i->visited = cc == bc::CNE ? ZYDIS_MNEMONIC_JNZ : ZYDIS_MNEMONIC_JZ;
					return nullptr;
				} else {
					// Can't have constant on LHS.
					//
					bool swapped = false;
					if (lhs->is<constant>()) {
						swapped = true;
						std::swap(lhs, rhs);
					}

					// Map the operator.
					//
					if (cc == bc::CLT) {
						i->visited = swapped ? ZYDIS_MNEMONIC_JNBE : ZYDIS_MNEMONIC_JB;
					} else if (cc == bc::CGE) {
						i->visited = swapped ? ZYDIS_MNEMONIC_JBE : ZYDIS_MNEMONIC_JNB;
					} else if (cc == bc::CGT) {
						i->visited = swapped ? ZYDIS_MNEMONIC_JB : ZYDIS_MNEMONIC_JNBE;
					} else {
						i->visited = swapped ? ZYDIS_MNEMONIC_JNB : ZYDIS_MNEMONIC_JBE;
					}

					// Handle operands.
					//
					#if __AVX__
					auto op = ZYDIS_MNEMONIC_VUCOMISD;
					#else
					auto op = ZYDIS_MNEMONIC_UCOMISD;
					#endif
					if (rhs->is<constant>()) {
						mc(op, to_reg(lhs, false), mc.ref_const(rhs->as<constant>()->to_any().coerce_num()));
					} else {
						mc(op, to_reg(lhs, false), to_reg(rhs, false));
					}
					return nullptr;
				}
			}
			case opcode::move: {
				auto val = i->operands[0];
				if (val->is<constant>()) {
					bool gp     = val->as<constant>()->vt != type::f64;
					auto result = reg.get_anyreg(ip, i->name, gp, true);
					if (!result)
						return string::create(mc.ibb->proc->L, "register allocation failed.");

					if (gp) {
						mc(ZYDIS_MNEMONIC_MOV, result, val->as<constant>()->to_any().value);
					} else {
						mc.move(result, mc.ref_const(val->as<constant>()->to_any().value));
					}
					return nullptr;
				} else {
					auto vn     = val->as<insn>()->name;
					auto vr     = reg.get_anyreg(ip, vn, reg.ideal_reg_type(vn));
					auto result = reg.get_anyreg(ip, i->name, arch::is_gp(vr), true);
					if (!vr || !result)
						return string::create(mc.ibb->proc->L, "register allocation failed.");
					mc.move(result, vr);
					return nullptr;
				}
			}
			case opcode::jcc: {
				auto cc = i->operands[0];

				ZydisMnemonic mnemonic;
				if (auto m = cc->as<insn>()->visited) {
					mnemonic = ZydisMnemonic(m);
				} else {
					auto vn = cc->as<insn>()->name;
					auto vr = reg.get_anyreg(ip, vn, true);
					mc(ZYDIS_MNEMONIC_TEST, vr, vr);
					mnemonic = ZYDIS_MNEMONIC_JNZ;
				}

				mc(mnemonic, i->operands[1]->as<constant>()->bb->uid);
				mc(ZYDIS_MNEMONIC_JMP, i->operands[2]->as<constant>()->bb->uid); // TODO: Very dumb.
				return nullptr;
			}
			case opcode::jmp:
				mc(ZYDIS_MNEMONIC_JMP, i->operands[0]->as<constant>()->bb->uid);
				return nullptr;

			case opcode::coerce_cast: {
				LI_ASSERT(i->operands[1]->as<constant>()->irtype == type::i1);

				auto vn  = i->operands[0]->as<insn>()->name;
				auto vr = reg.get_anyreg(ip, vn, reg.ideal_reg_type(vn));
				auto tmp = reg.alloc_next(ip, true);
				if (!vr || !tmp)
					return string::create(mc.ibb->proc->L, "register allocation failed.");

				mc.move(tmp, vr);
				mc(ZYDIS_MNEMONIC_NOT, tmp);
				mc(ZYDIS_MNEMONIC_SHR, tmp, 47);
				mc(ZYDIS_MNEMONIC_ADD, tmp, -10);
				mc(ZYDIS_MNEMONIC_CMP, tmp, -2);
				i->visited = ZYDIS_MNEMONIC_JB;
				return nullptr;
			}
			case opcode::binop: {
				auto op   = i->operands[0]->as<constant>()->vmopr;
				auto calc = [&](auto& vl, auto& vr) -> string* {
					// Get output register.
					//
					auto vx = reg.get_anyreg(ip, ip, false, true);
					switch (op) {
						case bc::AADD: {
	#if __AVX__
							mc(ZYDIS_MNEMONIC_VADDSD, vx, vl, vr);
	#else
							mc(ZYDIS_MNEMONIC_MOVSD, vx, lh);
							mc(ZYDIS_MNEMONIC_ADDSD, vx, rh);
	#endif
							return nullptr;
						}
						case bc::ASUB: {
	#if __AVX__
							mc(ZYDIS_MNEMONIC_VSUBSD, vx, vl, vr);
	#else
							mc(ZYDIS_MNEMONIC_MOVSD, vx, lh);
							mc(ZYDIS_MNEMONIC_SUBSD, vx, rh);
	#endif
							return nullptr;
						}
						case bc::AMUL: {
	#if __AVX__
							mc(ZYDIS_MNEMONIC_VMULSD, vx, vl, vr);
	#else
							mc(ZYDIS_MNEMONIC_MOVSD, vx, lh);
							mc(ZYDIS_MNEMONIC_MULSD, vx, rh);
	#endif
							return nullptr;
						}
						case bc::ADIV: {
	#if __AVX__
							mc(ZYDIS_MNEMONIC_VDIVSD, vx, vl, vr);
	#else
							mc(ZYDIS_MNEMONIC_MOVSD, vx, lh);
							mc(ZYDIS_MNEMONIC_DIVSD, vx, rh);
	#endif
							return nullptr;
						}
						case bc::AMOD: {
	#if __AVX__
							mc(ZYDIS_MNEMONIC_VDIVSD, vx, vl, vr);
							mc(ZYDIS_MNEMONIC_VROUNDSD, vx, vx, vx, 11);  // = trunc(x/y)
							mc(ZYDIS_MNEMONIC_VMULSD, vx, vx, vr);
							mc(ZYDIS_MNEMONIC_VSUBSD, vx, vl, vx);
	#else
							auto vt = reg.alloc_next(ip, false);
							mc(ZYDIS_MNEMONIC_MOVSD, vx, vl);
							mc(ZYDIS_MNEMONIC_MOVSD, vt, vl);
							mc(ZYDIS_MNEMONIC_DIVSD, vt, vr);
							mc(ZYDIS_MNEMONIC_ROUNDSD, vt, vt, 11);
							mc(ZYDIS_MNEMONIC_MULSD, vt, vr);
							mc(ZYDIS_MNEMONIC_SUBSD, vx, vt);
	#endif
							return nullptr;
						}
						default:
							return string::format(mc.ibb->proc->L, "Binop can't be lowered, call C: '%s'", i->to_string(true).c_str());
					}
				};

				// If LHS is a constant, try fixing it.
				//
				auto lhs = i->operands[1].get();
				auto rhs = i->operands[2].get();
				if (lhs->is<constant>()) {
					if (op == bc::AMUL || op == bc::AADD)
						std::swap(lhs, rhs);
				}

				// RHS can be a constant.
				//
				if (rhs->is<constant>()) {
					auto lh = lhs->as<insn>()->name;
					auto vl = reg.get_anyreg(ip, lh, false);
					auto vr = mc.ref_const(rhs->as<constant>()->to_any().coerce_num());
					return calc(vl, vr);
				}
				// If LHS is a constant, we need to load it into a temporary.
				//
				else if (lhs->is<constant>()) {
					auto rh = rhs->as<insn>()->name;
					auto vr = reg.get_anyreg(ip, rh, false);
					auto pl = mc.ref_const(lhs->as<constant>()->to_any().coerce_num());
					auto vl = reg.alloc_next(ip, false, 1);
					mc.move(vl, pl);
					return calc(vl, vr);
				}
				// Otherwise, if both are registers, all good.
				//
				else {
					auto lh = lhs->as<insn>()->name;
					auto vl = reg.get_anyreg(ip, lh, false);
					auto rh = rhs->as<insn>()->name;
					auto vr = reg.get_anyreg(ip, rh, false);
					return calc(vl, vr);
				}
			}

			case opcode::thrw:
			case opcode::ret: {
				// Set pending epilogue flag.
				//
				mc.pending_epilogue = true;

				// Store the result.
				//
				auto res = i->operands[0]->as<insn>();
				auto in_reg = reg.get_anyreg(ip, res->name, reg.ideal_reg_type(res->name));
				mc.move(zy::mem{.size = 8, .base = arch::bp, .disp = FRAME_RET * 8}, in_reg);

				// Set the ok flag.
				//
				if (i->opc == opcode::thrw)
					mc(ZYDIS_MNEMONIC_MOV, zy::EAX, 1);
				else
					mc(ZYDIS_MNEMONIC_XOR, zy::EAX, zy::EAX);
				return nullptr;
			}
			default:
				return string::format(mc.ibb->proc->L, "Instruction NYI: '%s'", i->to_string(true).c_str());
		}
	}

	// Initial codegen with no profiling information.
	// - On success, writes the JIT callback into function and returns null.
	// - On failure, returns the error reason.
	//
	string* generate_code(procedure* proc_proto) {
		// Duplicate the procedure as we will transform it out of SSA, effectively making it invalid.
		//
		auto proc = proc_proto->duplicate();

		// Debug.
		//
		printf("------------------------------------------------------------------------------------\n");
		printf("----------------------------------- JIT Input IR -----------------------------------\n");
		printf("------------------------------------------------------------------------------------\n\n");
		proc_proto->print();
		printf("------------------------------------------------------------------------------------\n\n");
		printf("Introducing aliasing and lowering away from SSA...\n\n");

		// Create the register allocator.
		//
		reg_allocator r{init_regalloc(proc.get())};

		printf("------------------------------------------------------------------------------------\n");
		printf("----------------------------------- JIT Ready IR -----------------------------------\n");
		printf("------------------------------------------------------------------------------------\n\n");
		proc->print();
		printf("------------------------------------------------------------------------------------\n");
		printf("--------------------------------- RegAlloc Intervals -------------------------------\n");
		printf("------------------------------------------------------------------------------------\n\n");
		r.print();
		printf("------------------------------------------------------------------------------------\n\n");

		// Create the MC procedure, implement the reg allocator callbacks.
		//
		mc_procedure mproc{.next_label = proc->next_block_uid};

		auto store_cb = [&](arch::reg reg, uint32_t slot) { mproc.blocks.back().move(zy::mem{.base = arch::sp, .disp = arch::shadow_stack + slot * 8}, reg); };
		auto load_cb  = [&](arch::reg reg, uint32_t slot) { mproc.blocks.back().move(reg, zy::mem{.base = arch::sp, .disp = arch::shadow_stack + slot * 8}); };
		auto move_cb  = [&](arch::reg dst, arch::reg src) { mproc.blocks.back().move(dst, src); };
		r.store       = store_cb;
		r.load        = load_cb;
		r.move        = move_cb;

		// Linearly iterate the topologically sorted basic blocks.
		//
		uint32_t ip = 0;
		for (auto& bb : proc->basic_blocks) {
			auto& mblk = mproc.add_block(bb.get());
			for (auto i : *bb) {
				//puts(i->to_string(true).c_str());
				// If there is debug info, and it is not equal to the last entry, push.
				//
				if (i->source_bc != bc::no_pos) {
					if (mblk.bc_to_ip.empty() || mblk.bc_to_ip.back().first != i->source_bc) {
						mblk.bc_to_ip.emplace_back(i->source_bc, mblk.code.size());
					}
				}

				// Try lowering the instruction.
				//
				size_t n = mblk.code.size();
				if (string* err = lower(mblk, r, i, ip)) {
					return err;
				}
				auto gen = std::span<const uint8_t>(mblk.code).subspan(n);
				while (auto i = zy::decode(gen)) {
					puts(i->to_string().c_str());
				}
				++ip;
			}
		}

		// Demo.
		//
		//printf("\n");
		//uint32_t index = 0;
		//for (auto& bb : proc->basic_blocks) {
		//	printf("-- Block $%u", bb->uid);
		//	if (bb->cold_hint)
		//		printf(LI_CYN " [COLD %u]" LI_DEF, (uint32_t) bb->cold_hint);
		//	putchar('\n');
		//
		//	for (auto i : *bb) {
		//		arch::reg result_reg = -999;
		//		if (i->vt != type::none && !i->is<compare>() /*lolz*/) {
		//			result_reg = r.get_anyreg(index, i->name, true);
		//		}
		//
		//		std::vector<arch::reg> arg_regs = {};
		//		for (auto& op : i->operands) {
		//			if (!op->is<constant>()) {
		//				arg_regs.emplace_back(r.get_anyreg(index, op->as<insn>()->name, true));
		//			} else {
		//				arg_regs.emplace_back(-999);
		//			}
		//		}
		//		printf(LI_GRN "#%-5x" LI_DEF " %s {%02x}", i->source_bc, i->to_string(true).c_str(), index);
		//		if (result_reg != -999)
		//			printf(" # => %s", arch::name_native(arch::to_native(result_reg)));
		//		else
		//			printf(" # => NULL");
		//		for (size_t k = 0; k != arg_regs.size(); k++) {
		//			if (arg_regs[k] != -999) {
		//				printf(" - %s", arch::name_native(arch::to_native(arg_regs[k])));
		//			}
		//		}
		//		putchar('\n');
		//		++index;
		//	}
		//}
		return string::create(proc_proto->L, "unknown JIT error");
	}
};



#endif