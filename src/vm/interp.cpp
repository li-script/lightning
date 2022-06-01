#include <lang/operator.hpp>
#include <vm/array.hpp>
#include <vm/bc.hpp>
#include <vm/function.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>

namespace li {
	enum vm_state : uint8_t {
		vm_exception,
		vm_ok,
		vm_begin,
	};

	// VM helpers.
	//
#define UNOP_HANDLE(K)                                      \
	case K: {                                                \
		auto [r, ok] = apply_unary(L, REG(b), K);             \
		if (!ok) [[unlikely]]                                 \
			VM_RET(r, true);                                   \
		dirty_stack(); /*TODO: not really unless metacalled*/ \
		REG(a) = r;                                           \
		continue;                                             \
	}
#define BINOP_HANDLE(K)                                     \
	case K: {                                                \
		auto [r, ok] = apply_binary(L, REG(b), REG(c), K);    \
		if (!ok) [[unlikely]]                                 \
			VM_RET(r, true);                                   \
		dirty_stack(); /*TODO: not really unless metacalled*/ \
		REG(a) = r;                                           \
		continue;                                             \
	}
#define VM_RET(value, ex)                                          \
	{                                                               \
		stack[locals_begin + FRAME_RET] = value;                     \
		state                           = ex ? vm_exception : vm_ok; \
		goto vret;                                                   \
	}
#if !LI_DEBUG
	#define REG(...)  stack[locals_begin + (__VA_ARGS__)]
	#define UVAL(...) f->uvals()[(__VA_ARGS__)]
	#define KVAL(...) f->kvals()[(__VA_ARGS__)]
#endif

	LI_NOINLINE static bool vm_loop(vm* __restrict L, call_frame frame, vm_state state) {
		// Cache stack locally, we will update the reference if we call into anything with side-effects.
		//
		any* __restrict stack = nullptr;
		auto dirty_stack      = [&]() LI_INLINE { stack = L->stack; };
		dirty_stack();

		// VM loop:
		//
		while (true) {
			// If function entry:
			//
			function* f = nullptr;
			slot_t    locals_begin;
			slot_t    n_args = 0;
			int64_t   ip     = 0;
			if (state == vm_begin) {
				// Push the return frame for previous function.
				//
				call_frame caller     = frame;
				n_args                = caller.n_args;
				uint32_t caller_frame = caller.stack_pos;
				uint32_t caller_pc    = caller.caller_pc;
				L->push_stack(bit_cast<opaque>(caller));
				dirty_stack();
				locals_begin = L->stack_top;

				// Validate function.
				//
				auto& vf = stack[L->stack_top + FRAME_TARGET];
				if (vf.is_traitful() && ((traitful_node<>*) vf.as_gc())->has_trait<trait::call>()) {
					stack[L->stack_top + FRAME_SELF] = vf;
					vf                               = vf.as_tbl()->get_trait<trait::call>();
				}
				if (vf.is_nfn()) {
					state = vf.as_nfn()->call(L, n_args, caller_frame, caller_pc) ? vm_ok : vm_exception;
					dirty_stack();
					goto vret;
				} else if (!vf.is_vfn()) [[unlikely]] {
					stack[L->stack_top + FRAME_RET] = string::create(L, "invoking non-function");
					state                           = vm_exception;
					goto vret;
				}

				// Validate argument count.
				//
				f = vf.as_vfn();
				if (f->num_arguments > n_args) [[unlikely]] {
					stack[L->stack_top + FRAME_RET] = string::format(L, "expected at least %u arguments, got %u", f->num_arguments, n_args);
					state                           = vm_exception;
					goto vret;
				}
				// Allocate locals and call.
				//
				else {
					L->alloc_stack(f->num_locals + MAX_ARGS);
					L->pop_stack_n(MAX_ARGS);
					dirty_stack();
				}
			}
			// If returning:
			//
			else {
				// If returning to C, return.
				//
				if (frame.multiplexed_by_c()) {
					return state == vm_ok;
				}

				// Unpack VM state.
				//
				locals_begin = frame.stack_pos;
				ip           = (int64_t) frame.caller_pc;
				any vf       = stack[locals_begin + FRAME_TARGET];
				LI_ASSERT(vf.is_vfn());
				f = vf.as_vfn();
#if LI_DEBUG
				n_args = bit_cast<call_frame>(stack[locals_begin + FRAME_CALLER].as_opq()).n_args;
#endif
			}

			// Enter execution scope.
			//
			{
				// Define debug helpers.
				//
#if LI_DEBUG
				auto REG = [&](bc::reg r) LI_INLINE -> any& {
					if (r < 0) {
						LI_ASSERT((n_args + FRAME_SIZE) >= (uint32_t) -r);
					} else {
						LI_ASSERT(f->num_locals > (uint32_t) r);
					}
					return stack[locals_begin + r];
				};
				auto UVAL = [&](bc::reg r) LI_INLINE -> any& {
					LI_ASSERT(f->num_uval > (uint32_t) r);
					return f->uvals()[r];
				};
				auto KVAL = [&](bc::reg r) LI_INLINE -> const any& {
					LI_ASSERT(f->num_kval > (uint32_t) r);
					return f->kvals()[r];
				};
#endif
				while (true) {
					const auto& insn   = f->opcode_array[ip++];
					auto [op, a, b, c] = insn;
					switch (op) {
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
							dirty_stack();  // meta
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
							if (err) {
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
							if (b == bc::uval_env) {
								REG(a) = any(f->environment);
							} else if (b == bc::uval_glb) {
								REG(a) = any(L->globals);
							} else {
								REG(a) = UVAL(b);
							}
							continue;
						}
						case bc::USET: {
							if (a == bc::uval_env) {
								auto tbl = REG(b);
								if (tbl.type() != type_table) [[unlikely]] {
									VM_RET(string::create(L, "can't use non-table as environment"), true);
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
								VM_RET(string::create(L, "indexing with null key"), true);
							}
							if (tbl.is_tbl()) {
								REG(a) = tbl.as_tbl()->get(L, REG(b));
							} else {
								VM_RET(string::create(L, "indexing non-table"), true);
							}
							continue;
						}
						case bc::TSETR: {
							auto& tbl = REG(c);
							auto  key = REG(a);
							auto  val = REG(b);

							if (key == none) [[unlikely]] {
								VM_RET(string::create(L, "indexing with null key"), true);
							}
							if (!tbl.is_tbl()) [[unlikely]] {
								if (tbl == none) {
									tbl = any{table::create(L)};
								} else [[unlikely]] {
									VM_RET(string::create(L, "indexing non-table"), true);
								}
							}
							if (tbl.as_tbl()->trait_freeze) [[unlikely]] {
								VM_RET(string::create(L, "modifying frozen table."), true);
							}

							tbl.as_tbl()->set(L, key, val);
							L->gc.tick(L);
							continue;
						}
						case bc::TGET: {
							auto tbl = REG(c);
							auto key = REG(b);
							if (key == none) [[unlikely]] {
								VM_RET(string::create(L, "indexing with null key"), true);
							}

							if (tbl.is_tbl()) {
								auto [r, ok] = tbl.as_tbl()->tget(L, REG(b));
								if (!ok) [[unlikely]] {
									VM_RET(r, true);
								}
								dirty_stack();
								REG(a) = r;
							} else if (tbl.is_arr()) {
								if (!key.is_num() || key.as_num() < 0) [[unlikely]] {
									VM_RET(string::create(L, "indexing array with non-integer or negative key"), true);
								}
								REG(a) = tbl.as_arr()->get(L, size_t(key.as_num()));
							} else if (tbl.is_str()) {
								if (!key.is_num() || key.as_num() < 0) [[unlikely]] {
									VM_RET(string::create(L, "indexing string with non-integer or negative key"), true);
								}
								auto i = size_t(key.as_num());
								auto v = tbl.as_str()->view();
								REG(a) = v.size() <= i ? any(none) : any(number((uint8_t)v[i]));
							} else if (tbl == none) {
								REG(a) = none;
							} else {
								VM_RET(string::create(L, "indexing non-table"), true);
							}
							continue;
						}
						case bc::TSET: {
							auto& tbl = REG(c);
							auto  key = REG(a);
							auto  val = REG(b);

							if (key == none) [[unlikely]] {
								VM_RET(string::create(L, "indexing with null key"), true);
							}
							if (tbl.is_arr()) {
								if (!key.is_num()) [[unlikely]] {
									VM_RET(string::create(L, "indexing array with non-integer key"), true);
								}
								if (!tbl.as_arr()->set(L, size_t(key.as_num()), val)) {
									VM_RET(string::create(L, "out-of-boundaries array access"), true);
								}
								continue;
							} else if (!tbl.is_tbl()) [[unlikely]] {
								if (tbl == none) {
									tbl = any{table::create(L)};
								} else [[unlikely]] {
									VM_RET(string::create(L, "indexing non-table"), true);
								}
							}

							auto [r, ok] = tbl.as_tbl()->tset(L, key, val);
							if (!ok) [[unlikely]] {
								VM_RET(r, true);
							}
							dirty_stack();
							L->gc.tick(L);
							continue;
						}
						case bc::GGET: {
							if (REG(b) == none) [[unlikely]] {
								VM_RET(string::create(L, "indexing with null key"), true);
							}
							REG(a) = f->environment->get(L, REG(b));
							dirty_stack();  // meta
							continue;
						}
						case bc::GSET: {
							if (REG(a) == none) [[unlikely]] {
								VM_RET(string::create(L, "indexing with null key"), true);
							}
							f->environment->set(L, REG(a), REG(b));
							dirty_stack();  // meta
							L->gc.tick(L);
							continue;
						}
						case bc::VJOIN: {
							auto src = REG(c);
							if (src.is_tbl()) {
								if (auto dst = REG(b); dst == none) {
									REG(a) = src.as_tbl()->duplicate(L);
								} else {
									REG(a) = dst;
									if (!dst.is_tbl()) [[unlikely]] {
										VM_RET(string::create(L, "can't join different types, expected table"), true);
									}
									auto* s = src.as_tbl();
									auto* d = dst.as_tbl();

									// Copy every trait except freeze.
									//
									d->trait_seal   = s->trait_seal;
									d->trait_hide   = s->trait_hide;
									d->trait_mask   = s->trait_mask;
									d->traits       = s->traits;

									// Copy fields raw.
									//
									for (auto& [k, v] : *s) {
										if (k != none)
											d->set(L, k, v);
									}
								}
							} else if (src.is_arr()) {
								auto dst = REG(b);
								REG(a) = dst;
								if (!dst.is_arr()) [[unlikely]] {
									VM_RET(string::create(L, "can't join different types, expected array"), true);
								}
								auto* s = src.as_arr();
								auto* d = dst.as_arr();

								size_t pos = d->size();
								d->resize(L, pos + s->size());
								memcpy(d->begin() + pos, s->begin(), s->size() * sizeof(any));
							} else {
								VM_RET(string::create(L, "join expected table or array"), true);
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
								} else if (value.is_vfn()) {
									value = value.as_vfn()->duplicate(L);
								} else if (value.is_nfn()) {
									value = L->duplicate(value.as_nfn());
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
							LI_ASSERT(fn.is_vfn());

							function* r = fn.as_vfn();
							if (r->num_uval) {
								r = r->duplicate(L);
								for (uint32_t i = 0; i != r->num_uval; i++) {
									r->uvals()[i] = REG(c + i);
								}
							}
							REG(a) = r;
							continue;
						}
						case bc::TRGET: {
							auto idx = trait(c);
							auto holder = REG(b);
							auto& trait  = REG(a);
							if (!holder.is_traitful()) {
								trait = none;
							} else {
								auto* t = (traitful_node<>*) holder.as_gc();
								trait = t->trait_hide ? none : t->get_trait(idx);
							}
							continue;
						}
						case bc::TRSET: {
							auto  idx    = trait(c);
							auto& holder = REG(a);
							auto  trait  = REG(b);
							if (!holder.is_traitful()) [[unlikely]] {
								if (holder == none) {
									holder = table::create(L);
								} else {
									VM_RET(string::create(L, "can't set traits on non-traitful type"), true);
								}
							}
							auto* t = (traitful_node<>*) holder.as_gc();
							if (auto ex = t->set_trait(L, idx, trait)) [[unlikely]] {
								VM_RET(string::create(L, ex), true);
							}
							continue;
						}
						case bc::CALL: {
							frame = {.stack_pos = locals_begin, .caller_pc = (uint32_t) ip, .n_args = a};
							goto vcall;
						}
						// Size availability guaranteed by +MAX_ARGUMENTS over-allocation.
						case bc::PUSHR:
							stack[L->stack_top++] = REG(a);
							continue;
						case bc::PUSHI:
							stack[L->stack_top++] = any(std::in_place, insn.xmm());
							continue;
						case bc::SLOAD:
							REG(a) = stack[L->stack_top - b];
							continue;
						case bc::SRST:
							// TODO: Hakc!
							if (state == vm_exception) {
								VM_RET(stack[L->stack_top + FRAME_RET], true);
							}
							L->stack_top = locals_begin + f->num_locals;
							continue;
						case bc::NOP:
							continue;
						default:
#if LI_DEBUG
							util::abort("unrecognized opcode '%02x'", (uint32_t) op);
#else
							assume_unreachable();
#endif
					}
				}
			}

			// Calling another function?
			//
		vcall:
			LI_ASSERT(stack == L->stack);
			state = vm_begin;
			continue;

		vret:
			LI_ASSERT(state != vm_begin);
			LI_ASSERT(stack == L->stack);

			// Reset stack.
			//
			L->stack_top = locals_begin;

			// If not returning to C, recurse.
			//
			frame = bit_cast<call_frame>(stack[locals_begin + FRAME_CALLER].as_opq());
			if (frame.multiplexed_by_c()) {
				return state == vm_ok;
			}
		}
	}

	// Caller must push all arguments in reverse order, then the self argument or none and the function itself.
	// - Caller frame takes the caller's base of stack and the PC receives the "return pointer".
	//
	LI_INLINE bool vm::call(slot_t n_args, slot_t caller_frame, uint32_t caller_pc) { return vm_loop(this, call_frame{.stack_pos = caller_frame, .caller_pc = caller_pc, .n_args = n_args}, vm_begin); }
};