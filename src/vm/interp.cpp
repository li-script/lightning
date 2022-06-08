#include <lang/operator.hpp>
#include <vm/array.hpp>
#include <vm/bc.hpp>
#include <vm/function.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>

namespace li {
	// VM helpers.
	//
#define VM_RET(value, ex)                     \
	do {                                       \
		locals_begin[FRAME_RET] = value;        \
		this->stack_top         = locals_begin; \
		return !ex;                             \
	} while (0)
#define UNOP_HANDLE(K)                             \
	case K: {                                       \
		auto [r, ok] = apply_unary(this, REG(b), K); \
		if (!ok) [[unlikely]]                        \
			VM_RET(r, true);                          \
		REG(a) = r;                                  \
		continue;                                    \
	}
#define BINOP_HANDLE(K)                                     \
	case K: {                                                \
		auto [r, ok] = apply_binary(this, REG(b), REG(c), K); \
		if (!ok) [[unlikely]]                                 \
			VM_RET(r, true);                                   \
		REG(a) = r;                                           \
		continue;                                             \
	}

#if !LI_DEBUG
	#define REG(...)  locals_begin[(__VA_ARGS__)]
	#define UVAL(...) f->upvalue_array[(__VA_ARGS__)]
	#define KVAL(...) f->proto->kvals()[(__VA_ARGS__)]
#endif

	LI_NOINLINE bool vm::call(any* args, slot_t n_args) {
		// Push the return frame for previous function.
		//
		auto       caller       = bit_cast<call_frame>(peek_stack().as_opq());
		msize_t    caller_frame = caller.stack_pos;
		msize_t    caller_pc    = caller.caller_pc;
		LI_ASSERT(&args[2] == &stack_top[FRAME_TARGET]);
		any* __restrict locals_begin = args + FRAME_SIZE + 1;

		// Validate function.
		//
		auto& vf = locals_begin[FRAME_TARGET];
		if (vf.is_traitful() && ((traitful_node<>*) vf.as_gc())->has_trait<trait::call>()) {
			locals_begin[FRAME_SELF] = vf;
			vf                       = vf.as_tbl()->get_trait<trait::call>();
		}
		if (!vf.is_fn()) [[unlikely]] {
			locals_begin[FRAME_RET] = string::format(this, "invoking non-function");
			return false;
		}
		function* f = vf.as_fn();
		if (f->is_native() || f->invoke != &vm_invoke) {
			// Swap previous c-frame.
			//
			call_frame prevframe = this->last_vm_caller;
			this->last_vm_caller    = call_frame{.stack_pos = caller_frame, .caller_pc = caller_pc};

			// Invoke callback.
			//
			auto* prev = this->stack_top;
			bool  ok   = vf.as_fn()->invoke(this, args, n_args);
			LI_ASSERT(this->stack_top == prev);  // Must be balanced!

			// Restore C-frame and return.
			//
			this->last_vm_caller = prevframe;
			return ok;
		}

		// Validate argument count.
		//
		if (f->num_arguments > n_args) [[unlikely]] {
			locals_begin[FRAME_RET] = string::format(this, "expected at least %u arguments, got %u", f->num_arguments, n_args);
			return false;
		}

		// Allocate stack space.
		//
		this->alloc_stack(f->proto->num_locals);

		// Define debug helpers.
		//
#if LI_DEBUG
		auto REG = [&](bc::reg r) LI_INLINE -> any& {
			if (r < 0) {
				LI_ASSERT((n_args + FRAME_SIZE) >= (msize_t) -r);
			} else {
				LI_ASSERT(f->proto->num_locals > (msize_t) r);
			}
			return locals_begin[r];
		};
		auto UVAL = [&](bc::reg r) LI_INLINE -> any& {
			LI_ASSERT(f->num_uval > (msize_t) r);
			return f->upvalue_array[r];
		};
		auto KVAL = [&](bc::reg r) LI_INLINE -> const any& {
			LI_ASSERT(f->proto->num_kval > (msize_t) r);
			return f->proto->kvals()[r];
		};
#endif
		const auto* __restrict opcode_array = &f->proto->opcode_array[0];
		const auto* __restrict ip           = opcode_array;
		while (true) {
			const auto& __restrict insn = *ip++;
			auto [op, a, b, c]          = insn;
			switch (op) {
				UNOP_HANDLE(bc::TOSTR)
				UNOP_HANDLE(bc::TONUM)
				UNOP_HANDLE(bc::TOINT)
				UNOP_HANDLE(bc::TOBOOL)
				UNOP_HANDLE(bc::LNOT)
				UNOP_HANDLE(bc::ANEG)
				UNOP_HANDLE(bc::VLEN)
				BINOP_HANDLE(bc::AADD)
				BINOP_HANDLE(bc::ASUB)
				BINOP_HANDLE(bc::AMUL)
				BINOP_HANDLE(bc::ADIV)
				BINOP_HANDLE(bc::AMOD)
				BINOP_HANDLE(bc::APOW)
				BINOP_HANDLE(bc::LAND)
				BINOP_HANDLE(bc::NCS)
				BINOP_HANDLE(bc::LOR)
				BINOP_HANDLE(bc::CEQ)
				BINOP_HANDLE(bc::CNE)
				BINOP_HANDLE(bc::CLT)
				BINOP_HANDLE(bc::CGT)
				BINOP_HANDLE(bc::CLE)
				BINOP_HANDLE(bc::CGE)
				BINOP_HANDLE(bc::VIN)

				case bc::CCAT: {
					REG(a) = string::concat(this, &REG(a), b);
					continue;
				}
				case bc::CTY: {
					REG(a) = to_canonical_type_name(REG(b).type()) == c;
					continue;
				}
				case bc::MOV: {
					REG(a) = REG(b);
					continue;
				}
				case bc::THRW:
					VM_RET(REG(a), true);
				case bc::RET:
					VM_RET(REG(a), false);
				case bc::JNS:
					if (REG(b).coerce_bool())
						continue;
					ip += a;
					continue;
				case bc::JS:
					if (!REG(b).coerce_bool())
						continue;
					ip += a;
					continue;
				case bc::JMP:
					ip += a;
					continue;
				case bc::ITER: {
					auto  target = REG(c);
					auto& iter   = REG(b + 0);
					auto& k      = REG(b + 1);
					auto& v      = REG(b + 2);

					// Get iterator state.
					//
					bool     err = false;
					bool     ok  = false;
					uint64_t it  = iter.as_opq().bits;

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
								iter = opaque{.bits = it + 1};

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
							auto*   e = t->data;
							for (; it < t->length; it++) {
								// Write the pair.
								//
								k = any(number(it));
								v = any(number(e[it]));

								// Update the iterator.
								//
								iter = opaque{.bits = it + 1};

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
									iter = opaque{.bits = it + 1};

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
							err = true;
							break;
					}
					if (err) [[unlikely]] {
						VM_RET(string::format(this, "cannot iterate %s", type_names[(uint8_t) target.type()]), true);
					}

					// If we did not find any, break.
					//
					if (!ok) {
						ip += a;
					}
					continue;
				}
				case bc::KIMM: {
					REG(a) = any(std::in_place, insn.xmm());
					continue;
				}
				case bc::UGET: {
					if (b == bc::uval_env) {
						REG(a) = any(f->environment);
					} else if (b == bc::uval_glb) {
						REG(a) = any(this->globals);
					} else {
						REG(a) = UVAL(b);
					}
					continue;
				}
				case bc::USET: {
					if (a == bc::uval_env) {
						auto tbl = REG(b);
						if (tbl.type() != type_table) [[unlikely]] {
							VM_RET(string::create(this, "can't use non-table as environment"), true);
						} else {
							f->environment = tbl.as_tbl();
						}
					} else {
						UVAL(a) = REG(b);
					}
					continue;
				}
				case bc::TGETR: {
					auto tbl = REG(c);
					auto key = REG(b);
					if (key == none) [[unlikely]] {
						VM_RET(string::create(this, "indexing with null key"), true);
					}

					if (tbl.is_tbl()) {
						REG(a) = tbl.as_tbl()->get(this, REG(b));
					} else if (tbl.is_arr()) {
						if (!key.is_num() || key.as_num() < 0) [[unlikely]] {
							VM_RET(string::create(this, "indexing array with non-integer or negative key"), true);
						}
						REG(a) = tbl.as_arr()->get(this, msize_t(key.as_num()));
					} else if (tbl == none) {
						REG(a) = none;
					} else {
						VM_RET(string::create(this, "indexing non-table"), true);
					}
					continue;
				}
				case bc::TSETR: {
					auto& tbl = REG(c);
					auto  key = REG(a);
					auto  val = REG(b);

					if (key == none) [[unlikely]] {
						VM_RET(string::create(this, "indexing with null key"), true);
					} else if (tbl == none) [[unlikely]] {
						tbl = any{table::create(this)};
					}

					if (tbl.is_tbl()) {
						if (tbl.as_tbl()->trait_freeze) [[unlikely]] {
							VM_RET(string::create(this, "modifying frozen table."), true);
						}
						tbl.as_tbl()->set(this, key, val);
						this->gc.tick(this);
					} else if (tbl.is_arr()) {
						if (!key.is_num() || key.as_num() < 0) [[unlikely]] {
							VM_RET(string::create(this, "indexing array with non-integer or negative key"), true);
						}
						if (!tbl.as_arr()->set(this, msize_t(key.as_num()), val)) [[unlikely]] {
							VM_RET(string::create(this, "out-of-boundaries array access"), true);
						}
					} else [[unlikely]] {
						VM_RET(string::create(this, "indexing non-table"), true);
					}
					continue;
				}
				case bc::TGET: {
					auto tbl = REG(c);
					auto key = REG(b);
					if (key == none) [[unlikely]] {
						VM_RET(string::create(this, "indexing with null key"), true);
					}

					if (tbl.is_tbl()) {
						auto [r, ok] = tbl.as_tbl()->tget(this, REG(b));
						if (!ok) [[unlikely]] {
							VM_RET(r, true);
						}
						REG(a) = r;
					} else if (tbl.is_arr()) {
						if (!key.is_num() || key.as_num() < 0) [[unlikely]] {
							VM_RET(string::create(this, "indexing array with non-integer or negative key"), true);
						}
						REG(a) = tbl.as_arr()->get(this, msize_t(key.as_num()));
					} else if (tbl.is_str()) {
						if (!key.is_num() || key.as_num() < 0) [[unlikely]] {
							VM_RET(string::create(this, "indexing string with non-integer or negative key"), true);
						}
						auto i = size_t(key.as_num());
						auto v = tbl.as_str()->view();
						REG(a) = v.size() <= i ? any(none) : any(number((uint8_t) v[i]));
					} else if (tbl == none) {
						REG(a) = none;
					} else {
						VM_RET(string::create(this, "indexing non-table"), true);
					}
					continue;
				}
				case bc::TSET: {
					auto& tbl = REG(c);
					auto  key = REG(a);
					auto  val = REG(b);

					if (key == none) [[unlikely]] {
						VM_RET(string::create(this, "indexing with null key"), true);
					}
					if (tbl.is_arr()) {
						if (!key.is_num()) [[unlikely]] {
							VM_RET(string::create(this, "indexing array with non-integer key"), true);
						}
						if (!tbl.as_arr()->set(this, msize_t(key.as_num()), val)) [[unlikely]] {
							VM_RET(string::create(this, "out-of-boundaries array access"), true);
						}
						continue;
					} else if (!tbl.is_tbl()) [[unlikely]] {
						if (tbl == none) {
							tbl = any{table::create(this)};
						} else [[unlikely]] {
							VM_RET(string::create(this, "indexing non-table"), true);
						}
					}

					auto [r, ok] = tbl.as_tbl()->tset(this, key, val);
					if (!ok) [[unlikely]] {
						VM_RET(r, true);
					}
					this->gc.tick(this);
					continue;
				}
				case bc::GGET: {
					if (REG(b) == none) [[unlikely]] {
						VM_RET(string::create(this, "indexing with null key"), true);
					}
					REG(a) = f->environment->get(this, REG(b));
					continue;
				}
				case bc::GSET: {
					if (REG(a) == none) [[unlikely]] {
						VM_RET(string::create(this, "indexing with null key"), true);
					}
					f->environment->set(this, REG(a), REG(b));
					this->gc.tick(this);
					continue;
				}
				case bc::VJOIN: {
					this->gc.tick(this);
					auto src = REG(c);
					if (src.is_tbl()) {
						if (auto dst = REG(b); dst == none) {
							REG(a) = src.as_tbl()->duplicate(this);
						} else {
							REG(a) = dst;
							if (!dst.is_tbl()) [[unlikely]] {
								VM_RET(string::create(this, "can't join different types, expected table"), true);
							}
							dst.as_tbl()->join(this, src.as_tbl());
						}
					} else if (src.is_arr()) {
						auto dst = REG(b);
						REG(a)   = dst;
						if (!dst.is_arr()) [[unlikely]] {
							VM_RET(string::create(this, "can't join different types, expected array"), true);
						}
						dst.as_arr()->join(this, src.as_arr());
					} else if (src.is_str()) {
						auto dst = REG(b);
						if (!dst.is_str()) [[unlikely]] {
							VM_RET(string::create(this, "can't join different types, expected string"), true);
						}
						REG(a) = string::concat(this, dst.as_str(), src.as_str());
					} else [[unlikely]] {
						VM_RET(string::create(this, "join expected table, array, or string"), true);
					}
					continue;
				}
				case bc::VDUP: {
					any value = REG(b);
					if (value.is_gc() && !value.is_str()) {
						if (value.is_arr()) {
							value = value.as_arr()->duplicate(this);
						} else if (value.is_tbl()) {
							value = value.as_tbl()->duplicate(this);
						} else if (value.is_fn()) {
							value = value.as_fn()->duplicate(this);
						} else {
							// TODO: Thread, userdata
						}
					}
					REG(a) = value;
					this->gc.tick(this);
					continue;
				}
				case bc::ANEW: {
					this->gc.tick(this);
					REG(a) = any{array::create(this, b)};
					continue;
				}
				case bc::ADUP: {
					this->gc.tick(this);
					auto arr = KVAL(b);
					LI_ASSERT(arr.is_arr());
					REG(a) = arr.as_arr()->duplicate(this);
					continue;
				}
				case bc::TNEW: {
					this->gc.tick(this);
					REG(a) = any{table::create(this, b)};
					continue;
				}
				case bc::TDUP: {
					this->gc.tick(this);
					auto tbl = KVAL(b);
					LI_ASSERT(tbl.is_tbl());
					REG(a) = tbl.as_tbl()->duplicate(this);
					continue;
				}
				case bc::FDUP: {
					this->gc.tick(this);
					auto fn = KVAL(b);
					LI_ASSERT(fn.is_fn());

					function* r = fn.as_fn()->duplicate(this);
					for (msize_t i = 0; i != r->num_uval; i++) {
						r->uvals()[i] = REG(c + i);
					}
					REG(a) = r;
					continue;
				}
				case bc::TRGET: {
					auto  idx    = trait(c);
					auto  holder = REG(b);
					auto& trait  = REG(a);
					if (!holder.is_traitful()) {
						trait = none;
					} else {
						auto* t = (traitful_node<>*) holder.as_gc();
						trait   = t->trait_hide ? none : t->get_trait(idx);
					}
					continue;
				}
				case bc::TRSET: {
					auto  idx    = trait(c);
					auto& holder = REG(a);
					auto  trait  = REG(b);
					if (!holder.is_traitful()) [[unlikely]] {
						if (holder == none) {
							holder = table::create(this);
						} else [[unlikely]] {
							VM_RET(string::create(this, "can't set traits on non-traitful type"), true);
						}
					}
					auto* t = (traitful_node<>*) holder.as_gc();
					if (auto ex = t->set_trait(this, idx, trait)) [[unlikely]] {
						VM_RET(string::create(this, ex), true);
					}
					continue;
				}
				case bc::CALL: {
					call_frame cf{.stack_pos = msize_t(locals_begin - this->stack), .caller_pc = msize_t(ip - opcode_array)};
					auto       argspace = this->stack_top - 2;
					this->push_stack(REG(b));
					this->push_stack(bit_cast<opaque>(cf));
					if (!this->call(argspace, a)) {
						VM_RET(this->stack_top[FRAME_RET], true);
					}
					continue;
				}
				// Size availability guaranteed by +MAX_ARGUMENTS over-allocation.
				case bc::PUSHR:
					this->push_stack(REG(a));
					continue;
				case bc::PUSHI:
					this->push_stack(any(std::in_place, insn.xmm()));
					continue;
				case bc::SLOAD:
					REG(a) = this->stack_top[-b];
					continue;
				case bc::SRST:
					this->stack_top = locals_begin + f->proto->num_locals;
					continue;
				case bc::NOP:
					continue;
				default:
#if LI_DEBUG
					util::abort("unrecognized opcode '%02x'", (msize_t) op);
#else
					assume_unreachable();
#endif
			}
		}
	}

	// nfunc_t for virtual functions.
	//
	bool vm_invoke(vm* L, any* args, slot_t n_args) {
		return L->call(args, n_args);
	}
};