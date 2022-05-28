#include <vm/state.hpp>
#include <lang/operator.hpp>
#include <vm/bc.hpp>
#include <vm/function.hpp>
#include <vm/array.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>

namespace li {
	// Caller must push all arguments in reverse order, then the self argument or none and the function itself.
	// - Caller frame takes the caller's base of stack and the PC receives the "return pointer".
	//
	LI_FLATTEN bool vm::call(uint32_t n_args, uint32_t caller_frame, uint32_t caller_pc) {
		call_frame retframe{.stack_pos = caller_frame, .caller_pc = caller_pc, .n_args = n_args};
		push_stack(bit_cast<iopaque>(retframe));

		// Reference the function and declare return helper.
		//
		uint32_t locals_begin = stack_top;
		uint32_t return_slot  = locals_begin + stack_ret;
		auto     fv           = stack[locals_begin + stack_fn];
		auto     ret          = [&](any value, bool is_exception) LI_INLINE {
         stack[return_slot] = value;
         stack_top          = locals_begin;
         return !is_exception;
		};

		// Validate type and argument count.
		//
		if (fv.is_nfn())
			return fv.as_nfn()->call(this, n_args, caller_frame, caller_pc);
		if (!fv.is_vfn()) [[unlikely]]  // TODO: Meta
			return ret(string::create(this, "invoking non-function"), true);
		auto* f = fv.as_vfn();
		if (f->num_arguments != n_args)
			return ret(string::format(this, "expected %u arguments, got %u", f->num_arguments, n_args), true);

		// Locals.
		//
		alloc_stack(f->num_locals);
		uint32_t reset_pos = stack_top;

		// Return and ref helpers.
		//
		auto ref_reg = [&](bc::reg r) LI_INLINE -> any& {
			if (r < 0) {
				LI_ASSERT((n_args+stack_rsvd) >= (uint32_t) -r);
			} else {
				LI_ASSERT(f->num_locals > (uint32_t) r);
			}
			return stack[locals_begin + r];
		};
		auto ref_uval = [&](bc::reg r) LI_INLINE -> any& {
			LI_ASSERT(f->num_uval > (uint32_t) r);
			return f->uvals()[r];
		};
		auto ref_kval = [&](bc::reg r) LI_INLINE -> const any& {
			LI_ASSERT(f->num_kval > (uint32_t) r);
			return f->kvals()[r];
		};

		for (uint32_t ip = 0;;) {
			const auto& insn   = f->opcode_array[ip++];
			auto [op, a, b, c] = insn;

			switch (op) {
				#define UNOP_SPECIALIZE(K) case K: {					  \
					auto [r, ok] = apply_unary(this, ref_reg(b), K);  \
					if (!ok) [[unlikely]]									  \
						return ret(r, true);									  \
					ref_reg(a) = r;											  \
					continue;													  \
				}
				#define BINOP_SPECIALIZE(K) case K: {										 \
					auto [r, ok] = apply_binary(this, ref_reg(b), ref_reg(c), K);	 \
					if (!ok) [[unlikely]]														 \
						return ret(r, true);														 \
					ref_reg(a) = r;																 \
					continue;																		 \
				}																						 
				UNOP_SPECIALIZE( bc::LNOT )
				UNOP_SPECIALIZE( bc::ANEG )
				BINOP_SPECIALIZE( bc::AADD )
				BINOP_SPECIALIZE( bc::ASUB )
				BINOP_SPECIALIZE( bc::AMUL )
				BINOP_SPECIALIZE( bc::ADIV )
				BINOP_SPECIALIZE( bc::AMOD )
				BINOP_SPECIALIZE( bc::APOW )
				BINOP_SPECIALIZE( bc::LAND )
				BINOP_SPECIALIZE( bc::NCS )
				BINOP_SPECIALIZE( bc::LOR )
				BINOP_SPECIALIZE( bc::CEQ )
				BINOP_SPECIALIZE( bc::CNE )
				BINOP_SPECIALIZE( bc::CLT )
				BINOP_SPECIALIZE( bc::CGT )
				BINOP_SPECIALIZE( bc::CLE )
				BINOP_SPECIALIZE( bc::CGE )

				case bc::CTY: {
					ref_reg(a) = to_canonical_type_name(ref_reg(b).type()) == c;
					continue;
				}
				case bc::CMOV: {
					if (ref_reg(c).as_bool())
						ref_reg(a) = ref_reg(b);
					continue;
				}
				case bc::MOV: {
					ref_reg(a) = ref_reg(b);
					continue;
				}
				case bc::THRW: {
					return ret(ref_reg(a), true);
				}
				case bc::RET: {
					return ret(ref_reg(a), false);
				}
				case bc::JNS:
					if (ref_reg(b).as_bool())
						continue;
					ip += a;
					continue;
				case bc::JS:
					if (!ref_reg(b).as_bool())
						continue;
					ip += a;
					continue;
				case bc::JMP:
					ip += a;
					continue;
				case bc::ITER: {
					auto  target = ref_reg(c);
					auto& iter   = ref_reg(b + 0);
					auto& k      = ref_reg(b + 1);
					auto& v      = ref_reg(b + 2);

					// Get iterator state.
					//
					bool     ok = false;
					uint64_t it = iter.as_opq().bits;

					switch (target.type()) {
						// Alias to empty table.
						//
						case type_none:
							ip += a;
							continue;

						// Array:
						//
						case type_array: {
							array* t = target.as_arr();
							auto*  e = t->begin();
							for (; it < t->length; it++) {
								// Write the pair.
								//
								k = any(number(it));
								v = any(e[it]);

								// Update the iterator.
								//
								iter = iopaque{.bits = it + 1};

								// Break.
								//
								ok = true;
								break;
							}
							break;
						}

						// String:
						//
						case type_string: {
							string* t = target.as_str();
							auto*  e = t->data;
							for (; it < t->length; it++) {
								// Write the pair.
								//
								k = any(number(it));
								v = any(number(e[it]));

								// Update the iterator.
								//
								iter = iopaque{.bits = it + 1};

								// Break.
								//
								ok = true;
								break;
							}
							break;
						}

						// Table:
						//
						case type_table: {
							table* t = target.as_tbl();
							auto*  e = t->begin();
							for (; it < (t->size() + overflow_factor); it++) {
								if (e[it].key != none) {
									// Write the pair.
									//
									k = e[it].key;
									v = e[it].value;

									// Update the iterator.
									//
									iter = iopaque{.bits = it + 1};

									// Break.
									//
									ok = true;
									break;
								}
							}
							break;
						}

						// Raise an error.
						//
						default:
							return ret(string::format(this, "cannot iterate %s", type_names[(uint8_t) target.type()]), true);
							break;
					}

					// If we did not find any, break.
					//
					if (!ok) {
						ip += a;
					}
					continue;
				}
				case bc::KIMM: {
					ref_reg(a) = any(std::in_place, insn.xmm());
					continue;
				}
				case bc::UGET: {
					if (b == bc::uval_env) {
						ref_reg(a) = any(f->environment);
					} else if (b == bc::uval_glb) {
						ref_reg(a) = any(globals);
					} else {
						ref_reg(a) = ref_uval(b);
					}
					continue;
				}
				case bc::USET: {
					if (a == bc::uval_env) {
						auto tbl       = ref_reg(b);
						if (tbl.type() != type_table) {
							return ret(string::create(this, "can't use non-table as environment"), true);
						}
						f->environment = tbl.as_tbl();
					} else {
						ref_uval(a) = ref_reg(b);
					}
					continue;
				}
				case bc::TGET: {
					auto tbl = ref_reg(c);
					auto key = ref_reg(b);

					if (tbl.is_tbl()) {
						ref_reg(a) = tbl.as_tbl()->get(this, ref_reg(b));
					} else if (tbl.is_arr()) {
						if (!key.is_num() || key.as_num() < 0) [[unlikely]] {
							return ret(string::create(this, "indexing array with non-integer or negative key"), true);
						}
						ref_reg(a) = tbl.as_arr()->get(this, size_t(key.as_num()));
					} else if (tbl.is_str()) {
						if (!key.is_num() || key.as_num() < 0) [[unlikely]] {
							return ret(string::create(this, "indexing string with non-integer or negative key"), true);
						}
						auto i     = size_t(key.as_num());
						auto v     = tbl.as_str()->view();
						ref_reg(a) = v.size() <= i ? any(none) : any(number(v[i])); 
					} else if (tbl.is(type_none)) {
						ref_reg(a) = none;
					} else {
						return ret(string::create(this, "indexing non-table"), true);
					} 
					continue;
				}
				case bc::TSET: {
					auto& tbl = ref_reg(c);
					auto  key = ref_reg(a);
					auto  val = ref_reg(b);

					if (key.is(type_none)) [[unlikely]] {
						return ret(string::create(this, "indexing with null key"), true);
					}
					if (tbl.is_arr()) {
						if (!key.is_num()) [[unlikely]] {
							return ret(string::create(this, "indexing array with non-integer key"), true);
						}
						if (!tbl.as_arr()->set(this, size_t(key.as_num()), val)) {
							return ret(string::create(this, "out-of-boundaries array access"), true);
						}
						continue;
					} 
					else if (!tbl.is_tbl()) [[unlikely]] {
						if (tbl.is(type_none)) {
							tbl = any{table::create(this)};
						} else {
							return ret(string::create(this, "indexing non-table"), true);
						}
					}
					tbl.as_tbl()->set(this, key, val);
					gc.tick(this);
					continue;
				}
				case bc::GGET: {
					ref_reg(a) = f->environment->get(this, ref_reg(b));
					continue;
				}
				case bc::GSET: {
					f->environment->set(this, ref_reg(a), ref_reg(b));
					gc.tick(this);
					continue;
				}
				case bc::ANEW: {
					gc.tick(this);
					ref_reg(a) = any{array::create(this, b)};
					continue;
				}
				case bc::ADUP: {
					gc.tick(this);
					auto arr = ref_kval(b);
					LI_ASSERT(arr.is_arr());
					ref_reg(a) = arr.as_arr()->duplicate(this);
					continue;
				}
				case bc::TNEW: {
					gc.tick(this);
					ref_reg(a) = any{table::create(this, b)};
					continue;
				}
				case bc::TDUP: {
					gc.tick(this);
					auto tbl = ref_kval(b);
					LI_ASSERT(tbl.is_tbl());
					ref_reg(a) = tbl.as_tbl()->duplicate(this);
					continue;
				}
				case bc::FDUP: {
					gc.tick(this);
					auto fn = ref_kval(b);
					LI_ASSERT(fn.is_vfn());

					function* r = fn.as_vfn();
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
					if (!call(a, locals_begin, ip))
						return ret(stack[stack_top + stack_ret], true);
					continue;
				case bc::PUSHR:
					push_stack(ref_reg(a));
					continue;
				case bc::PUSHI:
					push_stack(any(std::in_place, insn.xmm()));
					continue;
				case bc::SLOAD:
					ref_reg(a) = stack[stack_top - b];
					continue;
				case bc::SRST:
					stack_top = reset_pos;
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