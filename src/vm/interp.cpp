#include <vm/state.hpp>
#include <lang/operator.hpp>
#include <vm/bc.hpp>
#include <vm/function.hpp>
#include <vm/array.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>

namespace li {
	// Calls the function at the first slot in callsite with the arguments following it
	// - Returns true on success and false if the VM throws an exception.
	// - In either case and will replace the function with a value representing
	//    either the exception or the result.
	//
	LI_FLATTEN bool vm::call(uint32_t callsite, uint32_t n_args) {
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
		if (fv.is(type_nfunction))
			return fv.as_nfn()->call(this, callsite, n_args);
		if (!fv.is(type_function)) [[unlikely]]  // TODO: Meta
			return ret(string::create(this, "invoking non-function"), true);
		auto* f = fv.as_vfn();

		// Allocate arguments and locals.
		//
		uint32_t cargs_begin = alloc_stack(f->num_arguments + f->num_locals);
		uint32_t locals_begin = cargs_begin + f->num_arguments;
		for (uint32_t a = 0; a != f->num_arguments; a++) {
			any v = a >= n_args ? none : stack[args_begin + a];
			stack[locals_begin - 1 - a] = v;
		}

		// Return and ref helpers.
		//
		auto ref_reg = [&](bc::reg r) LI_INLINE -> any& {
			if (r < 0) {
				LI_ASSERT(f->num_arguments >= (uint32_t) -r);
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
					if (auto& e = ref_reg(a); e != none)
						return ret(e, true);
					continue;
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
					if (b == bc::uval_fun) {
						ref_reg(a) = any(f);
					} else if (b == bc::uval_env) {
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

					if (tbl.is(type_table)) {
						ref_reg(a) = tbl.as_tbl()->get(this, ref_reg(b));
					} else if (tbl.is(type_array)) {
						if (!key.is(type_number)) [[unlikely]] {
							return ret(string::create(this, "indexing array with non-integer key"), true);
						}
						ref_reg(a) = tbl.as_arr()->get(this, size_t(key.as_num()));
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

					if (tbl.is(type_array)) {
						if (!key.is(type_number)) [[unlikely]] {
							return ret(string::create(this, "indexing array with non-integer key"), true);
						}
						if (!tbl.as_arr()->set(this, size_t(key.as_num()), val)) {
							return ret(string::create(this, "out-of-boundaries array access"), true);
						}
						continue;
					} 
					else if (!tbl.is(type_table)) [[unlikely]] {
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
					LI_ASSERT(arr.is(type_array));
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
					LI_ASSERT(tbl.is(type_table));
					ref_reg(a) = tbl.as_tbl()->duplicate(this);
					continue;
				}
				case bc::FDUP: {
					gc.tick(this);
					auto fn = ref_kval(b);
					LI_ASSERT(fn.is(type_function));

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
					LI_ASSERT(a >= 0 && uint32_t(a + b + 1) <= f->num_locals);
					if (!call(locals_begin + a, b))
						return ret(stack[locals_begin + a], true);
					continue;
				case bc::INVK:
					LI_ASSERT(b >= 0 && uint32_t(b + c + 1) <= f->num_locals);
					if (!call(locals_begin + b, c))
						ip += a;
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