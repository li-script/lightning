#include <vm/state.hpp>
#include <lang/operator.hpp>
#include <vm/bc.hpp>
#include <vm/function.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>

namespace lightning::core {
	// Calls the function at the first slot in callsite with the arguments following it
	// - Returns true on success and false if the VM throws an exception.
	// - In either case and will replace the function with a value representing
	//    either the exception or the result.
	//
	bool vm::call(uint32_t callsite, uint32_t n_args) {
		/*
			<fn>
			arg0
			arg1
			...
			argn
			<locals of caller ...>
			<locals of this function> -> <retval>
		*/

		// Declare frame bounds and return helper.
		//
		const uint32_t stack_frame = stack_top;
		const uint32_t args_begin  = callsite + 1;
		auto ret = [&](any value, bool is_exception) LI_INLINE {
			stack_top = stack_frame;
         stack[callsite] = value;
			return !is_exception;
		};

		// Reference the function.
		//
		auto fv = stack[callsite];
		if (!fv.is(type_function)) [[unlikely]]  // TODO: Meta
			return ret(string::create(this, "invoking non-function"), true);
		auto* f = fv.as_fun();

		// Allocate locals.
		//
		uint32_t locals_begin = alloc_stack(f->num_locals);

		// Return and ref helpers.
		//
		auto ref_reg = [&](bc::reg r) LI_INLINE -> any& {
			if (r >= 0) {
				LI_ASSERT(f->num_locals > r);
				return stack[locals_begin + r];
			} else {
				r = -(r + 1);
				// TODO: Arg# does not have to match :(
				return stack[args_begin + r];
			}
		};
		auto ref_uval = [&](bc::reg r) LI_INLINE -> any& {
			LI_ASSERT(f->num_uval > r);
			return f->uvals()[r];
		};
		auto ref_kval = [&](bc::reg r) LI_INLINE -> const any& {
			LI_ASSERT(f->num_kval > r);
			return f->kvals()[r];
		};


		for (uint32_t ip = 0;;) {
			const auto& insn   = f->opcode_array[ip++];
			auto [op, a, b, c] = insn;

			switch (op) {
				case bc::TYPE:
				case bc::LNOT:
				case bc::ANEG: {
					auto [r, ok] = apply_unary(this, ref_reg(b), op);
					if (!ok) [[unlikely]]
						return ret(r, true);
					ref_reg(a) = r;
					continue;
				}
				case bc::AADD:
				case bc::ASUB:
				case bc::AMUL:
				case bc::ADIV:
				case bc::AMOD:
				case bc::APOW:
				case bc::LAND:
				case bc::LOR:
				case bc::SCAT:
				case bc::CEQ:
				case bc::CNE:
				case bc::CLT:
				case bc::CGT:
				case bc::CLE:
				case bc::CGE: {
					auto [r, ok] = apply_binary(this, ref_reg(b), ref_reg(c), op);
					if (!ok) [[unlikely]]
						return ret(r, true);
					ref_reg(a) = r;
					continue;
				}
				case bc::CMOV: {
					ref_reg(a) = ref_reg(b).as_bool() ? ref_reg(c) : none;
					continue;
				}
				case bc::MOV: {
					ref_reg(a) = ref_reg(b);
					continue;
				}
				case bc::THRW: {
					if (auto& e = ref_reg(a); e != none)
						return ret(e, true);
					continue;
				}
				case bc::RETN: {
					return ret(ref_reg(a), false);
				}
				case bc::JCC:
					if (!ref_reg(b).as_bool())
						continue;
					[[fallthrough]];
				case bc::JMP:
					ip += a;
					continue;
				case bc::KIMM: {
					ref_reg(a) = any(std::in_place, insn.xmm());
					continue;
				}
				case bc::KGET: {
					ref_reg(a) = ref_kval(b);
					continue;
				}
				case bc::UGET: {
					ref_reg(a) = ref_uval(b);
					continue;
				}
				case bc::USET: {
					ref_uval(a) = ref_reg(b);
					continue;
				}
				case bc::TGET: {
					auto tbl = ref_reg(c);
					if (!tbl.is(type_table)) [[unlikely]] {
						if (tbl.is(type_none)) {
							ref_reg(a) = none;
							continue;
						}
						return ret(string::create(this, "indexing non-table"), true);
					}
					ref_reg(a) = tbl.as_tbl()->get(this, ref_reg(b));
					continue;
				}
				case bc::TSET: {
					auto& tbl = ref_reg(c);
					if (!tbl.is(type_table)) [[unlikely]] {
						if (tbl.is(type_none)) {
							tbl = any{table::create(this)};
						} else {
							return ret(string::create(this, "indexing non-table"), true);
						}
					}
					tbl.as_tbl()->set(this, ref_reg(a), ref_reg(b));
					continue;
				}
				case bc::GGET: {
					ref_reg(a) = f->environment->get(this, ref_reg(b));
					continue;
				}
				case bc::GSET: {
					f->environment->set(this, ref_reg(a), ref_reg(b));
					continue;
				}
				case bc::TNEW: {
					ref_reg(a) = any{table::create(this, b)};
					continue;
				}
				case bc::TDUP: {
					auto tbl = ref_kval(b);
					LI_ASSERT(tbl.is(type_table));
					ref_reg(a) = tbl.as_tbl()->duplicate(this);
					continue;
				}
				case bc::FDUP: {
					auto fn = ref_kval(b);
					LI_ASSERT(fn.is(type_function));

					function* r = fn.as_fun();
					if (r->num_uval) {
						r = r->duplicate(this);
						for (uint32_t i = 0; i != r->num_uval; i++) {
							r->uvals()[i] = ref_reg(c + i);
						}
					}
					ref_reg(a) = r;
					continue;
				}
				case bc::CALL:
					LI_ASSERT(a >= 0 && (a+b+1) <= f->num_locals);
					if (!call(locals_begin + a, b))
						return ret(stack[a], true);
					continue;
				case bc::INVK:
					LI_ASSERT(a >= 0 && (a+b+1) <= f->num_locals);
					if (!call(locals_begin + a, b))
						ip += c;
					continue;
				case bc::BP:
					breakpoint();
					continue;
				case bc::NOP:
					continue;
				default:
					util::abort("unrecognized bytecode '%02x'", (uint32_t) op);
					break;
			}
		}
	}
};