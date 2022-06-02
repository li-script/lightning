#include <ir/lifter.hpp>
#include <ir/value.hpp>
#include <ir/proc.hpp>
#include <ir/insn.hpp>

namespace li::ir {
	static void lift_basic_block(builder bld, const std::vector<basic_block*>& bc_to_bb) {
		// Local cache.
		//
		function*                           f = bld.blk->proc->f;
		std::vector<std::shared_ptr<value>> local_locals;
		local_locals.resize(f->num_locals + f->num_arguments + FRAME_SIZE);
		bc::reg local_shift = f->num_arguments + FRAME_SIZE;

		auto get_kval = [&](bc::reg r) { return bld.blk->proc->f->kvals()[r]; };
		auto set_reg = [&]<typename T>(bc::reg r, T&& v) { local_locals[r + local_shift] = insn::value_launder(std::forward<T>(v)); };
		auto get_reg  = [&](bc::reg r) -> std::shared_ptr<value> {
         auto& x = local_locals[r + local_shift];
         if (!x) {
				x = bld.emit<load_local>(r);
				if (r == FRAME_TARGET)
					x = bld.emit<assume_cast>(std::move(x), type::vfn);
			}
         return x;
		};
		auto this_func = [&]() { return get_reg(FRAME_TARGET); };
		auto spill = [&]() {
			for (bc::reg r = 0; r != local_locals.size(); r++) {
				auto& x = local_locals[r];
				if (!x)
					continue;
				if (x->is<insn>()) {
					auto* i = x->get<insn>();
					if (i->op == opcode::load_local && i->operands[0]->get<constant>()->i == (r - local_shift))
						continue;
				}
				bld.emit<store_local>(r - local_shift, x);
			}
		};

		bc::pos ip      = bld.blk->bc_begin;
		bc::pos ip_end  = bld.blk->bc_end;
		auto    opcodes = f->opcodes();
		while (ip != ip_end) {
			bld.current_bc      = ip;
			auto& insn          = opcodes[ip++];
			auto& [op, a, b, c] = insn;

			switch (op) {
				// Unop.
				//
				case bc::ANEG: {
					set_reg(a, bld.emit<unop>(op, get_reg(b)));
					continue;
				}
				case bc::LNOT: {
					set_reg(a, bld.emit<lnot>(op, get_reg(b)));
					continue;
				}
				case bc::VLEN: {
					set_reg(a, bld.emit<vlen>(op, get_reg(b)));
					continue;
				}

				// Binop.
				//
				case bc::AADD:
				case bc::ASUB:
				case bc::AMUL:
				case bc::ADIV:
				case bc::AMOD:
				case bc::APOW: {
					set_reg(a, bld.emit<binop>(op, get_reg(b), get_reg(c)));
					continue;
				}

				// Data transfer.
				//
				case bc::KIMM: {
					set_reg(a, any(std::in_place, insn.xmm()));
					continue;
				}
				case bc::MOV: {
					set_reg(a, get_reg(b));
					continue;
				}

				// Logical ops.
				//
				case bc::LAND: {
					set_reg(a, bld.emit<land>(op, get_reg(b), get_reg(c)));
					continue;
				}
				case bc::LOR: {
					set_reg(a, bld.emit<lor>(op, get_reg(b), get_reg(c)));
					continue;
				}
				case bc::NCS: {
					set_reg(a, bld.emit<null_coalesce>(op, get_reg(b), get_reg(c)));
					continue;
				}
				case bc::CTY: {
					set_reg(a, bld.emit<test_type>(op, get_reg(b), (value_type) c));
					continue;
				}
				case bc::CGT:
				case bc::CGE:
				case bc::CNE:
				case bc::CLE:
				case bc::CLT:
				case bc::CEQ: {
					set_reg(a, bld.emit<compare>(op, get_reg(b), get_reg(c)));
					continue;
				}

				// Type specials.
				//
				case bc::VIN: {
					set_reg(a, bld.emit<vin>(get_reg(b), get_reg(c)));
					continue;
				}
				case bc::VJOIN: {
					set_reg(a, bld.emit<vjoin>(get_reg(b), get_reg(c)));
					continue;
				}
				case bc::ANEW: {
					set_reg(a, bld.emit<array_new>(b));
					continue;
				}
				case bc::TNEW: {
					set_reg(a, bld.emit<table_new>(b));
					continue;
				}
				case bc::VDUP:
				case bc::ADUP:
				case bc::TDUP: {
					set_reg(a, bld.emit<dup>(get_kval(b)));
					continue;
				}
				// case bc::CCAT: // A=CONCAT(A..A+B)

				// Traits:
				//
				case bc::TRGET: {
					set_reg(a, bld.emit<trait_get>(get_reg(b), trait(c)));
					continue;
				}
				case bc::TRSET: {
					bld.emit<trait_set>(get_reg(a), trait(c), get_reg(b));
					continue;
				}

				// Upvalue and closures:
				//
				case bc::FDUP: {
					auto bf     = get_kval(b);
					auto r  = bld.emit<dup>(bf);
					r           = bld.emit<assume_cast>(r, type::vfn);
					for (bc::reg i = 0; i != bf.as_vfn()->num_uval; i++) {
						bld.emit<uval_set>(r, i, get_reg(c + i));
					}
					set_reg(a, r);
					continue;
				}
				case bc::UGET: {
					set_reg(a, bld.emit<uval_get>(this_func(), b));
					continue;
				}
				case bc::USET: {
					bld.emit<uval_set>(this_func(), b, get_reg(a));
					continue;
				}

				// Tables:
				//
				case bc::TGET: {
					set_reg(a, bld.emit<field_get>(get_reg(c), get_reg(b)));
					continue;
				}
				case bc::TGETR: {
					set_reg(a, bld.emit<field_get_raw>(get_reg(c), get_reg(b)));
					continue;
				}
				case bc::TSET: {
					bld.emit<field_set>(get_reg(c), get_reg(a), get_reg(b));
					continue;
				}
				case bc::TSETR: {
					bld.emit<field_set_raw>(get_reg(c), get_reg(a), get_reg(b));
					continue;
				}
				case bc::GGET: {
					auto g = bld.emit<uval_get>(this_func(), bc::uval_env);
					set_reg(a, bld.emit<field_get>(g, get_reg(b)));
					continue;
				}
				case bc::GSET: {
					auto g = bld.emit<uval_get>(this_func(), bc::uval_env);
					bld.emit<field_set>(g, get_reg(a), get_reg(b));
					continue;
				}

				// Control flow:
				//
				case bc::RET: {
					bld.emit<ret>(get_reg(a));
					return;
				}
				case bc::THRW: {
					bld.emit<thrw>(get_reg(a));
					return;
				}
				case bc::JS:
				case bc::JNS: {
					auto tf = bc_to_bb[ip];
					auto tt = bc_to_bb[ip + a];
					if (op == bc::JNS)
						std::swap(tt, tf);
					spill();
					bld.emit<jcc>(get_reg(b), tt, tf);
					bld.blk->proc->add_jump(bld.blk, tt);
					bld.blk->proc->add_jump(bld.blk, tf);
					return;
				}
				case bc::JMP: {
					auto tt = bc_to_bb[ip + a];
					spill();
					bld.emit<jmp>(tt);
					bld.blk->proc->add_jump(bld.blk, tt);
					return;
				}
				//case bc::ITER: {
				//	auto tt = bc_to_bb[ip + a];
				//
				//}
				// _(ITER, rel, reg, reg, none) B[1,2] = C[B++].kv, JMP A if end

				/*
				Stack operators.
				_(PUSHR, reg, ___, ___, none) PUSH(A)
				_(PUSHI, ___, xmm, ___, none) PUSH(A)
				_(SLOAD, reg, sp, ___, none)  A = STACK[TOP-B]
				_(SRST, ___, ___, ___, none)  Resets the stack pos
				Control flow.
				_(CALL, imm, ___, ___, none)   A = Arg count
				*/
				default:
					util::abort("Opcode %s NYI\n", bc::opcode_details(op).name);
					return;
			}
		}

		// If terminated because of a label, jump to continuation.
		//
		auto tt = bc_to_bb[ip];
		spill();
		bld.emit<jmp>(tt);
		bld.blk->proc->add_jump(bld.blk, tt);
	}

	// Step #1:
	// Generates crude bytecode using load_local and store_local.
	//
	std::unique_ptr<procedure> lift_bc(vm* L, function* f) {
		auto proc = std::make_unique<procedure>(L, f);

		// Bytecode label position to basic-block mapping.
		//
		std::vector<basic_block*> bc_to_bb = {};
		
		// Determine all labels.
		//
		bc_to_bb.resize(f->length);
		auto add_label = [&](bc::pos ip) {
			basic_block*& b = bc_to_bb[ip];
			if (!b) {
				b = proc->add_block();
				b->bc_begin = ip;
			}
		};
		add_label(0);
		for (bc::pos i = 0; i != f->length; i++) {
			auto ip = i + 1;
			if (f->opcode_array[i].o == bc::JMP) {
				add_label(ip + f->opcode_array[i].a);
			} else if (f->opcode_array[i].o == bc::JS || f->opcode_array[i].o == bc::JNS || f->opcode_array[i].o == bc::ITER) {
				add_label(ip);
				add_label(ip + f->opcode_array[i].a);
			}
		}

		// Determine all basic block ranges.
		//
		for (auto& block : proc->basic_blocks) {
			bc::pos end = block->bc_begin + 1;
			while (end < bc_to_bb.size() && !bc_to_bb[end]) {
				end++;
			}
			block->bc_end = end;
		}

		// Lift all blocks.
		//
		for (auto& block : proc->basic_blocks) {
			lift_basic_block(block.get(), bc_to_bb);
		}

		// Topologically sort and return the procedure.
		//
		proc->toplogical_sort();
		return proc;
	}
};