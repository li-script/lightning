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
#define VM_RET(value, ex)                                    \
	do {                                                      \
		if (ex && catchpad_i) [[unlikely]] {                   \
			*catchpad_v  = value;                               \
			ip           = catchpad_i;                          \
			L->stack_top = locals_begin + f->proto->num_locals; \
			continue;                                           \
		}                                                      \
		locals_begin[FRAME_RET] = value;                       \
		L->stack_top            = locals_begin;                \
		return !ex;                                            \
	} while (0)
#define UNOP_HANDLE(K)                          \
	case K: {                                    \
		auto [r, ok] = apply_unary(L, REG(b), K); \
		if (!ok) [[unlikely]]                     \
			VM_RET(r, true);                       \
		REG(a) = r;                               \
		continue;                                 \
	}
#define BINOP_HANDLE(K)                                  \
	case K: {                                             \
		auto [r, ok] = apply_binary(L, REG(b), REG(c), K); \
		if (!ok) [[unlikely]]                              \
			VM_RET(r, true);                                \
		REG(a) = r;                                        \
		continue;                                          \
	}

#if !LI_DEBUG
	#define REG(...)  locals_begin[(__VA_ARGS__)]
	#define UVAL(...) f->upvalue_array[(__VA_ARGS__)]
	#define KVAL(...) f->proto->kvals()[(__VA_ARGS__)]
#endif
	LI_NOINLINE bool vm_invoke(vm* L, any* args, slot_t n_args) {
		// Push the return frame for previous function.
		//
		auto caller = bit_cast<call_frame>(L->peek_stack().as_opq());
		LI_ASSERT(&args[2] == &L->stack_top[FRAME_TARGET]);
		any* __restrict locals_begin = args + FRAME_SIZE + 1;

		// Validate function.
		//
		auto& vf = locals_begin[FRAME_TARGET];
		if (vf.is_traitful() && ((traitful_node<>*) vf.as_gc())->has_trait<trait::call>()) {
			locals_begin[FRAME_SELF] = vf;
			vf                       = vf.as_tbl()->get_trait<trait::call>();
		}
		if (!vf.is_fn()) [[unlikely]] {
			locals_begin[FRAME_RET] = string::format(L, "invoking non-function");
			return false;
		}
		function* f = vf.as_fn();
		if (f->invoke != &vm_invoke) {
			return f->invoke(L, args, n_args);
		}

		// Validate argument count.
		//
		if (f->proto->num_arguments > n_args) [[unlikely]] {
			locals_begin[FRAME_RET] = string::format(L, "expected at least %u arguments, got %u", f->proto->num_arguments, n_args);
			return false;
		}

		// Allocate stack space.
		//
		L->alloc_stack(f->proto->num_locals);

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
		const bc::insn* __restrict catchpad_i = nullptr;
		any* catchpad_v                       = nullptr;
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
					REG(a) = string::concat(L, &REG(a), b);
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
						case type_nil:
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
								if (e[it].key != nil) {
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
						VM_RET(string::format(L, "cannot iterate %s", type_names[(uint8_t) target.type()]), true);
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
					REG(a) = UVAL(b);
					continue;
				}
				case bc::USET: {
					UVAL(a) = REG(b);
					continue;
				}
				case bc::TGETR: {
					auto tbl = REG(c);
					auto key = REG(b);
					if (key == nil) [[unlikely]] {
						VM_RET(string::create(L, "indexing with null key"), true);
					}

					if (tbl.is_tbl()) {
						REG(a) = tbl.as_tbl()->get(L, REG(b));
					} else if (tbl.is_arr()) {
						if (!key.is_num() || key.as_num() < 0) [[unlikely]] {
							VM_RET(string::create(L, "indexing array with non-integer or negative key"), true);
						}
						REG(a) = tbl.as_arr()->get(L, msize_t(key.as_num()));
					} else if (tbl == nil) {
						REG(a) = nil;
					} else {
						VM_RET(string::create(L, "indexing non-table"), true);
					}
					continue;
				}
				case bc::TSETR: {
					auto& tbl = REG(c);
					auto  key = REG(a);
					auto  val = REG(b);

					if (key == nil) [[unlikely]] {
						VM_RET(string::create(L, "indexing with null key"), true);
					} else if (tbl == nil) [[unlikely]] {
						tbl = any{table::create(L)};
					}

					if (tbl.is_tbl()) {
						if (tbl.as_tbl()->trait_freeze) [[unlikely]] {
							VM_RET(string::create(L, "modifying frozen table."), true);
						}
						tbl.as_tbl()->set(L, key, val);
						L->gc.tick(L);
					} else if (tbl.is_arr()) {
						if (!key.is_num() || key.as_num() < 0) [[unlikely]] {
							VM_RET(string::create(L, "indexing array with non-integer or negative key"), true);
						}
						if (!tbl.as_arr()->set(L, msize_t(key.as_num()), val)) [[unlikely]] {
							VM_RET(string::create(L, "out-of-boundaries array access"), true);
						}
					} else [[unlikely]] {
						VM_RET(string::create(L, "indexing non-table"), true);
					}
					continue;
				}
				case bc::TGET: {
					auto tbl = REG(c);
					auto key = REG(b);
					if (key == nil) [[unlikely]] {
						VM_RET(string::create(L, "indexing with null key"), true);
					}

					if (tbl.is_tbl()) {
						auto [r, ok] = tbl.as_tbl()->tget(L, REG(b));
						if (!ok) [[unlikely]] {
							VM_RET(r, true);
						}
						REG(a) = r;
					} else if (tbl.is_arr()) {
						if (!key.is_num() || key.as_num() < 0) [[unlikely]] {
							VM_RET(string::create(L, "indexing array with non-integer or negative key"), true);
						}
						REG(a) = tbl.as_arr()->get(L, msize_t(key.as_num()));
					} else if (tbl.is_str()) {
						if (!key.is_num() || key.as_num() < 0) [[unlikely]] {
							VM_RET(string::create(L, "indexing string with non-integer or negative key"), true);
						}
						auto i = size_t(key.as_num());
						auto v = tbl.as_str()->view();
						REG(a) = v.size() <= i ? any(nil) : any(number((uint8_t) v[i]));
					} else if (tbl == nil) {
						REG(a) = nil;
					} else {
						VM_RET(string::create(L, "indexing non-table"), true);
					}
					continue;
				}
				case bc::TSET: {
					auto& tbl = REG(c);
					auto  key = REG(a);
					auto  val = REG(b);

					if (key == nil) [[unlikely]] {
						VM_RET(string::create(L, "indexing with null key"), true);
					}
					if (tbl.is_arr()) {
						if (!key.is_num()) [[unlikely]] {
							VM_RET(string::create(L, "indexing array with non-integer key"), true);
						}
						if (!tbl.as_arr()->set(L, msize_t(key.as_num()), val)) [[unlikely]] {
							VM_RET(string::create(L, "out-of-boundaries array access"), true);
						}
						continue;
					} else if (!tbl.is_tbl()) [[unlikely]] {
						if (tbl == nil) {
							tbl = any{table::create(L)};
						} else [[unlikely]] {
							VM_RET(string::create(L, "indexing non-table"), true);
						}
					}

					auto [r, ok] = tbl.as_tbl()->tset(L, key, val);
					if (!ok) [[unlikely]] {
						VM_RET(r, true);
					}
					L->gc.tick(L);
					continue;
				}
				case bc::VJOIN: {
					L->gc.tick(L);
					auto src = REG(c);
					if (src.is_tbl()) {
						if (auto dst = REG(b); dst == nil) {
							REG(a) = src.as_tbl()->duplicate(L);
						} else {
							REG(a) = dst;
							if (!dst.is_tbl()) [[unlikely]] {
								VM_RET(string::create(L, "can't join different types, expected table"), true);
							}
							dst.as_tbl()->join(L, src.as_tbl());
						}
					} else if (src.is_arr()) {
						auto dst = REG(b);
						REG(a)   = dst;
						if (!dst.is_arr()) [[unlikely]] {
							VM_RET(string::create(L, "can't join different types, expected array"), true);
						}
						dst.as_arr()->join(L, src.as_arr());
					} else if (src.is_str()) {
						auto dst = REG(b);
						if (!dst.is_str()) [[unlikely]] {
							VM_RET(string::create(L, "can't join different types, expected string"), true);
						}
						REG(a) = string::concat(L, dst.as_str(), src.as_str());
					} else [[unlikely]] {
						VM_RET(string::create(L, "join expected table, array, or string"), true);
					}
					continue;
				}
				case bc::VDUP: {
					any value = REG(b);
					if (value.is_gc() && !value.is_str()) {
						if (value.is_arr()) {
							value = value.as_arr()->duplicate(L);
						} else if (value.is_tbl()) {
							value = value.as_tbl()->duplicate(L);
						} else if (value.is_fn()) {
							value = value.as_fn()->duplicate(L);
						} else {
							// TODO: Thread, userdata
						}
					}
					REG(a) = value;
					L->gc.tick(L);
					continue;
				}
				case bc::ANEW: {
					L->gc.tick(L);
					REG(a) = any{array::create(L, b)};
					continue;
				}
				case bc::ADUP: {
					L->gc.tick(L);
					auto arr = KVAL(b);
					LI_ASSERT(arr.is_arr());
					REG(a) = arr.as_arr()->duplicate(L);
					continue;
				}
				case bc::TNEW: {
					L->gc.tick(L);
					REG(a) = any{table::create(L, b)};
					continue;
				}
				case bc::TDUP: {
					L->gc.tick(L);
					auto tbl = KVAL(b);
					LI_ASSERT(tbl.is_tbl());
					REG(a) = tbl.as_tbl()->duplicate(L);
					continue;
				}
				case bc::FDUP: {
					L->gc.tick(L);
					auto fn = KVAL(b);
					LI_ASSERT(fn.is_fn());

					function* r = fn.as_fn()->duplicate(L);
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
						trait = nil;
					} else {
						auto* t = (traitful_node<>*) holder.as_gc();
						trait   = t->trait_hide ? nil : t->get_trait(idx);
					}
					continue;
				}
				case bc::TRSET: {
					auto  idx    = trait(c);
					auto& holder = REG(a);
					auto  trait  = REG(b);
					if (!holder.is_traitful()) [[unlikely]] {
						if (holder == nil) {
							holder = table::create(L);
						} else [[unlikely]] {
							VM_RET(string::create(L, "can't set traits on non-traitful type"), true);
						}
					}
					auto* t = (traitful_node<>*) holder.as_gc();
					if (auto ex = t->set_trait(L, idx, trait)) [[unlikely]] {
						VM_RET(string::create(L, ex), true);
					}
					continue;
				}
				case bc::SETEH: {
					if (a) {
						catchpad_i = ip + a;
						catchpad_v = &REG(b);
					} else {
						catchpad_i = nullptr;
						catchpad_v = nullptr;
					}
					continue;
				}
				case bc::CALL: {
					call_frame cf{.stack_pos = msize_t(locals_begin - L->stack), .caller_pc = msize_t(ip - opcode_array)};
					auto       argspace = L->stack_top - 2;
					L->push_stack(REG(b));
					L->push_stack(bit_cast<opaque>(cf));
					if (!vm_invoke(L, argspace, a)) [[unlikely]] {
						VM_RET(L->stack_top[FRAME_RET], true);
					}
					continue;
				}
				// Size availability guaranteed by +MAX_ARGUMENTS over-allocation.
				case bc::PUSHR:
					L->push_stack(REG(a));
					continue;
				case bc::PUSHI:
					L->push_stack(any(std::in_place, insn.xmm()));
					continue;
				case bc::SLOAD:
					REG(a) = L->stack_top[-b];
					continue;
				case bc::SRST:
					L->stack_top = locals_begin + f->proto->num_locals;
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
};