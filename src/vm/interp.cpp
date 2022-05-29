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
				L->push_stack(bit_cast<iopaque>(caller));
				dirty_stack();
				locals_begin = L->stack_top;

				// Validate function.
				//
				auto vf = stack[L->stack_top + FRAME_TARGET];
				f       = vf.as_vfn();
				if (vf.is_nfn()) {
					state = vf.as_nfn()->call(L, n_args, caller_frame, caller_pc) ? vm_ok : vm_exception;
					dirty_stack();
					goto vret;
				} else if (!vf.is_vfn()) [[unlikely]] {
					// TODO: Meta
					stack[L->stack_top + FRAME_RET] = string::create(L, "invoking non-function");
					state                           = vm_exception;
					goto vret;
				}
				// Validate argument count.
				//
				else if (f->num_arguments > n_args) [[unlikely]] {
					stack[L->stack_top + FRAME_RET] = string::format(L, "expected at least %u arguments, got %u", f->num_arguments, n_args);
					state                           = vm_exception;
					goto vret;
				}
				// Allocate locals and call.
				//
				else {
					L->alloc_stack(f->num_locals);
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

						case bc::CCAT: {
							REG(a) = string::concat(L, &REG(a), b);
							continue;
						}
						case bc::CTY: {
							REG(a) = to_canonical_type_name(REG(b).type()) == c;
							continue;
						}
						case bc::CMOV: {
							if (REG(c).as_bool())
								REG(a) = REG(b);
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
							if (REG(b).as_bool())
								continue;
							ip += a;
							continue;
						case bc::JS:
							if (!REG(b).as_bool())
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
									auto*   e = t->data;
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
						case bc::TGET: {
							auto tbl = REG(c);
							auto key = REG(b);

							if (tbl.is_tbl()) {
								REG(a) = tbl.as_tbl()->get(L, REG(b));
								dirty_stack();  // meta
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
							} else if (tbl.is(type_none)) {
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

							if (key.is(type_none)) [[unlikely]] {
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
								if (tbl.is(type_none)) {
									tbl = any{table::create(L)};
								} else [[unlikely]] {
									VM_RET(string::create(L, "indexing non-table"), true);
								}
							}
							tbl.as_tbl()->set(L, key, val);
							dirty_stack();  // meta
							L->gc.tick(L);
							continue;
						}
						case bc::GGET: {
							REG(a) = f->environment->get(L, REG(b));
							dirty_stack();  // meta
							continue;
						}
						case bc::GSET: {
							f->environment->set(L, REG(a), REG(b));
							dirty_stack();  // meta
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
						case bc::CALL: {
							frame = {.stack_pos = locals_begin, .caller_pc = (uint32_t) ip, .n_args = a};
							goto vcall;
						}
						case bc::PUSHR:
							L->push_stack(REG(a));
							dirty_stack();
							continue;
						case bc::PUSHI:
							L->push_stack(any(std::in_place, insn.xmm()));
							dirty_stack();
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
						case bc::BP:
							breakpoint();
							continue;
						case bc::NOP:
							continue;
						default:
							util::abort("unrecognized bytecode '%02x'", (uint32_t) op);
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