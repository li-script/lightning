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

		auto store_local_f = [&]<typename T>(bc::reg r, T&& v) { local_locals[r + local_shift] = insn::value_launder(std::forward<T>(v)); };
		auto load_local_f  = [&](bc::reg r) -> std::shared_ptr<value> {
         auto& x = local_locals[r + local_shift];
         if (!x)
            x = bld.emit<load_local>(r);
         return x;
		};
		auto spill_locals = [&]() {
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
		auto get_kval = [&](bc::reg r) { return bld.blk->proc->f->kvals()[r]; };

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
					store_local_f(a, bld.emit<unop>(op, load_local_f(b)));
					continue;
				}
				case bc::LNOT: {
					store_local_f(a, bld.emit<lnot>(op, load_local_f(b)));
					continue;
				}
				case bc::VLEN: {
					store_local_f(a, bld.emit<vlen>(op, load_local_f(b)));
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
					store_local_f(a, bld.emit<binop>(op, load_local_f(b), load_local_f(c)));
					continue;
				}

				// Data transfer.
				//
				case bc::KIMM: {
					store_local_f(a, any(std::in_place, insn.xmm()));
					continue;
				}
				case bc::MOV: {
					store_local_f(a, load_local_f(b));
					continue;
				}

				// Logical ops.
				//
				case bc::LAND: {
					store_local_f(a, bld.emit<land>(op, load_local_f(b), load_local_f(c)));
					continue;
				}
				case bc::LOR: {
					store_local_f(a, bld.emit<lor>(op, load_local_f(b), load_local_f(c)));
					continue;
				}
				case bc::NCS: {
					store_local_f(a, bld.emit<null_coalesce>(op, load_local_f(b), load_local_f(c)));
					continue;
				}
				case bc::CTY: {
					store_local_f(a, bld.emit<check_type>(op, load_local_f(b), (value_type) c));
					continue;
				}
				case bc::CGT:
				case bc::CGE:
				case bc::CNE:
				case bc::CLE:
				case bc::CLT:
				case bc::CEQ: {
					store_local_f(a, bld.emit<compare>(op, load_local_f(b), load_local_f(c)));
					continue;
				}

				// Type specials.
				//
				case bc::VIN: {
					store_local_f(a, bld.emit<vin>(load_local_f(b), load_local_f(c)));
					continue;
				}
				case bc::VJOIN: {
					store_local_f(a, bld.emit<vjoin>(load_local_f(b), load_local_f(c)));
					continue;
				}
				case bc::ANEW: {
					store_local_f(a, bld.emit<array_new>(b));
					continue;
				}
				case bc::TNEW: {
					store_local_f(a, bld.emit<table_new>(b));
					continue;
				}
				case bc::VDUP:
				case bc::ADUP:
				case bc::TDUP: {
					store_local_f(a, bld.emit<dup>(get_kval(b)));
					continue;
				}
				// case bc::CCAT: // A=CONCAT(A..A+B)

				// Traits:
				//
				case bc::TRGET: {
					store_local_f(a, bld.emit<trait_get>(load_local_f(b), trait(c)));
					continue;
				}
				case bc::TRSET: {
					bld.emit<trait_set>(load_local_f(a), trait(c), load_local_f(b));
					continue;
				}

				// Upvalue and closures:
				//
				// case bc::FDUP: // A=Duplicate(KVAL[B]), A.UVAL[0]=C, A.UVAL[1]=C+1..
				case bc::UGET: {
					store_local_f(a, bld.emit<uval_get>(b));
					continue;
				}
				case bc::USET: {
					bld.emit<uval_set>(b, load_local_f(a));
					continue;
				}

				// Tables:
				//
				case bc::TGET: {
					store_local_f(a, bld.emit<field_get>(load_local_f(c), load_local_f(b)));
					continue;
				}
				case bc::TGETR: {
					store_local_f(a, bld.emit<field_get_raw>(load_local_f(c), load_local_f(b)));
					continue;
				}
				case bc::TSET: {
					bld.emit<field_set>(load_local_f(c), load_local_f(a), load_local_f(b));
					continue;
				}
				case bc::TSETR: {
					bld.emit<field_set_raw>(load_local_f(c), load_local_f(a), load_local_f(b));
					continue;
				}
				// TODO: HOW to get ENV?
				// _(GGET, reg, reg, ___, none) /* A=G[B] */
				// _(GSET, reg, reg, ___, none) /* G[A]=B */

				// Control flow:
				//
				case bc::RET: {
					bld.emit<ret>(load_local_f(a));
					return;
				}
				case bc::THRW: {
					bld.emit<thrw>(load_local_f(a));
					return;
				}
				case bc::JS:
				case bc::JNS: {
					auto tf = bc_to_bb[ip];
					auto tt = bc_to_bb[ip + a];
					if (op == bc::JNS)
						std::swap(tt, tf);
					spill_locals();
					bld.emit<jcc>(load_local_f(b), tt, tf);
					bld.blk->proc->add_jump(bld.blk, tt);
					bld.blk->proc->add_jump(bld.blk, tf);
					return;
				}
				case bc::JMP: {
					auto tt = bc_to_bb[ip + a];
					spill_locals();
					bld.emit<jmp>(tt);
					bld.blk->proc->add_jump(bld.blk, tt);
					return;
				}
				// _(ITER, rel, reg, reg, none) B[1,2] = C[B].kv, JMP A if end

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
					printf("idk how to handle opcode %s\n", bc::opcode_details(op).name);
					return;
			}
		}

		// If terminated because of a label, jump to continuation.
		//
		auto tt = bc_to_bb[ip];
		spill_locals();
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
			} else if (f->opcode_array[i].o == bc::JS || f->opcode_array[i].o == bc::JNS) {
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