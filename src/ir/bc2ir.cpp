#include <ir/bc2ir.hpp>
#include <ir/value.hpp>
#include <ir/proc.hpp>
#include <ir/insn.hpp>

namespace li::ir {
	static void lift_basic_block(builder bld, const std::vector<basic_block*>& bc_to_bb) {
		// Local cache.
		//
		function_proto*         f = bld.blk->proc->f;
		std::vector<ref<value>> local_locals;
		local_locals.resize(f->num_locals + f->num_arguments + FRAME_SIZE);
		bc::reg local_shift = f->num_arguments + FRAME_SIZE;

		auto get_kval = [&](bc::reg r) { return bld.blk->proc->f->kvals()[r]; };
		auto set_reg  = [&]<typename T>(bc::reg r, T&& v) { local_locals[r + local_shift] = launder_value(bld.blk->proc, std::forward<T>(v)); };
		auto get_reg  = [&](bc::reg r) -> ref<value> {
         auto& x = local_locals[r + local_shift];
         if (!x) {
				x = bld.emit<load_local>(r);
				if (r == FRAME_TARGET)
					x = bld.emit<assume_cast>(std::move(x), type::fn);
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
					auto* i = x->as<insn>();
					if (i->is<load_local>() && i->operands[0]->as<constant>()->i == (r - local_shift))
						continue;
				}
				bld.emit<store_local>(r - local_shift, x);
			}
		};

		std::vector<use<>> call_args  = {};
		bc::pos            ip        = bld.blk->bc_begin;
		bc::pos            ip_end    = bld.blk->bc_end;
		auto               opcodes    = f->opcodes();
		while (ip != ip_end) {
			bld.current_bc      = ip;
			auto& insn          = opcodes[ip++];
			auto& [op, a, b, c] = insn;

			switch (op) {
				// Unop.
				//
				case bc::LNOT:
				case bc::ANEG: {
					set_reg(a, bld.emit<unop>(op, get_reg(b)));
					continue;
				}
				case bc::VLEN: {
					set_reg(a, bld.emit<vlen>(get_reg(b)));
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
					auto br = get_reg(b);
					bld.emit<select>(bld.emit<coerce_cast>(br, type::i1), get_reg(c), br);
					continue;
				}
				case bc::LOR: {
					auto br = get_reg(b);
					bld.emit<select>(bld.emit<coerce_cast>(br, type::i1), br, get_reg(c));
					continue;
				}
				case bc::NCS: {
					auto br = get_reg(b);
					bld.emit<select>(bld.emit<compare>(bc::CEQ, br, any(nil)), get_reg(c), br);
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
					bld.emit<gc_tick>();
					set_reg(a, bld.emit<vjoin>(get_reg(b), get_reg(c)));
					continue;
				}
				case bc::ANEW: {
					bld.emit<gc_tick>();
					set_reg(a, bld.emit<array_new>(b));
					continue;
				}
				case bc::TNEW: {
					bld.emit<gc_tick>();
					set_reg(a, bld.emit<table_new>(b));
					continue;
				}
				case bc::VDUP: {
					bld.emit<gc_tick>();
					set_reg(a, bld.emit<vdup>(get_reg(b)));
					continue;
				}
				case bc::ADUP:
				case bc::TDUP: {
					bld.emit<gc_tick>();
					set_reg(a, bld.emit<vdup>(any(std::in_place, insn.xmm())));
					continue;
				}
				case bc::CCAT: {
					bld.emit<gc_tick>();
					auto tmp = get_reg(a);
					for (msize_t i = 1; i != b; i++) {
						tmp = bld.emit<vjoin>(std::move(tmp), get_reg(a + i));
					}
					set_reg(a, std::move(tmp));
					continue;
				}

				// Casts:
				//
				case bc::TOSTR: {
					bld.emit<gc_tick>();
					set_reg(a, bld.emit<coerce_cast>(get_reg(b), type::str));
					continue;
				}
				case bc::TONUM: {
					set_reg(a, bld.emit<coerce_cast>(get_reg(b), type::f64));
					continue;
				}
				case bc::TOINT: {
					set_reg(a, bld.emit<coerce_cast>(get_reg(b), type::i32));
					continue;
				}
				case bc::TOBOOL: {
					set_reg(a, bld.emit<coerce_cast>(get_reg(b), type::i1));
					continue;
				}

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
					bld.emit<gc_tick>();
					auto bf     = get_kval(b);
					auto r  = bld.emit<vdup>(bf);
					r           = bld.emit<assume_cast>(r, type::fn);
					for (bc::reg i = 0; i != bf.as_fn()->num_uval; i++) {
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
					set_reg(a, bld.emit<field_get>(false, get_reg(c), get_reg(b)));
					continue;
				}
				case bc::TGETR: {
					set_reg(a, bld.emit<field_get>(true, get_reg(c), get_reg(b)));
					continue;
				}
				case bc::TSET: {
					bld.emit<gc_tick>();
					bld.emit<field_set>(false, get_reg(c), get_reg(a), get_reg(b));
					continue;
				}
				case bc::TSETR: {
					bld.emit<gc_tick>();
					bld.emit<field_set>(true, get_reg(c), get_reg(a), get_reg(b));
					continue;
				}

				// Virtual calls:
				//
				case bc::PUSHI: {
					call_args.emplace_back(launder_value(bld.blk->proc, any(std::in_place, insn.xmm())));
					continue;
				}
				case bc::PUSHR: {
					call_args.emplace_back(get_reg(a));
					continue;
				}
				case bc::CALL: {
					LI_ASSERT(call_args.size() == (b + 2));
					bld.blk->proc->max_stack_slot = std::max<msize_t>(msize_t(call_args.size() + 1), bld.blk->proc->max_stack_slot);
					auto vc                       = bld.emit<vcall>(std::move(*call_args.rbegin()), std::move(*std::next(call_args.rbegin())));
					vc->operands.insert(vc->operands.end(), std::make_move_iterator(std::next(call_args.rbegin(), 2)), std::make_move_iterator(call_args.rend()));
					set_reg(a, std::move(vc));
					call_args.clear();
					// TODO: Br to rethrow based on result
					continue;
				}
				// TODO:
				// case bc::SETEH: {
				//}
				case bc::SETEX: {
					bld.emit<set_exception>(get_reg(a));
					continue;
				}
				case bc::GETEX: {
					set_reg(a, bld.emit<get_exception>());
					continue;
				}

				// Control flow:
				//
				case bc::RET: {
					bld.emit<ret>(get_reg(a));
					return;
				}
				case bc::JS:
				case bc::JNS: {
					auto tf = bc_to_bb[ip];
					auto tt = bc_to_bb[ip + a];
					if (op == bc::JNS)
						std::swap(tt, tf);
					spill();
					auto cnd = get_reg(b);
					if (!cnd->is(type::i1))
						cnd = bld.emit<coerce_cast>(cnd, type::i1);
					bld.emit<jcc>(cnd, tt, tf);
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
				// _(ITER, rel, reg, reg, nil) B[1,2] = C[B++].kv, JMP A if end

				default:
					util::abort("Opcode %s NYI\n", bc::opcode_details(op).name);
					return;
			}
		}

		// Stack must be balanced.
		//
		LI_ASSERT(call_args.empty());

		// If terminated because of a label, jump to continuation.
		//
		auto tt = bc_to_bb[ip];
		spill();
		bld.emit<jmp>(tt);
		bld.blk->proc->add_jump(bld.blk, tt);
	}

	// Generates crude bytecode.
	//
	std::unique_ptr<procedure> lift_bc(vm* L, function_proto* f) {
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
		for (auto it = proc->basic_blocks.begin() + 1; it != proc->basic_blocks.end();) {
			if (it->get()->predecessors.empty()) {
				it = proc->del_block(it->get());
			} else {
				++it;
			}
		} 

		// Topologically sort and return the procedure.
		//
		proc->topological_sort();
		return proc;
	}
};