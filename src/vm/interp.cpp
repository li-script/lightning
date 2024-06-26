#include <lang/operator.hpp>
#include <vm/array.hpp>
#include <vm/bc.hpp>
#include <vm/function.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <vm/object.hpp>

namespace li {
	// VM helpers.
	//
#define VM_RETHROW()                 \
	{                                 \
		if (catchpad_i) {              \
			ip           = catchpad_i;  \
			L->stack_top = reset_point; \
			continue;                   \
		}                              \
		L->stack_top = locals_begin;   \
		return exception_marker; \
	}
#define VM_RET(value, ex)          \
	{                               \
		if (ex) [[unlikely]] {       \
			L->last_ex = value;       \
			VM_RETHROW();             \
		}                            \
		L->stack_top = locals_begin; \
		return L->ok(value);         \
	}																					

#define UNOP_HANDLE(K)                    \
	case K: {                              \
		auto r = apply_unary(L, REG(b), K); \
		if (r.is_exc()) [[unlikely]]        \
			VM_RETHROW();                    \
		REG(a) = r;                         \
		continue;                           \
	}
#define BINOP_HANDLE(K)                            \
	case K: {                                       \
		auto r = apply_binary(L, REG(b), REG(c), K); \
		if (r.is_exc()) [[unlikely]]                 \
			VM_RETHROW();                             \
		REG(a) = r;                                  \
		continue;                                    \
	}

#if !LI_DEBUG
	#define REG(...)  locals_begin[(__VA_ARGS__)]
	#define UVAL(...) f->uvals()[(__VA_ARGS__)]
	#define KVAL(...) f->proto->kvals()[(__VA_ARGS__)]
#endif
	any_t LI_CC vm_invoke(vm* L, any* args, slot_t n_args) {
		LI_ASSERT(&args[2] == &L->stack_top[FRAME_TARGET]);
		any* const __restrict locals_begin = args + FRAME_SIZE + 1;

		// Validate function.
		//
		auto& vf = locals_begin[FRAME_TARGET];
		if (vf.is_vcl()) {
			locals_begin[FRAME_SELF] = vf;
			vf                       = vf.as_vcl()->ctor;
		}
		if (!vf.is_fn()) [[unlikely]] {
			return L->error("invoking non-function");
		}
		function* f = vf.as_fn();
		if (f->invoke != &vm_invoke) {
			return f->invoke(L, args, n_args);
		}

		// Allocate stack space.
		//
		msize_t num_locals = f->proto->num_locals;
		auto reset_point = L->alloc_stack(num_locals) + num_locals;

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
			return f->uvals()[r];
		};
		auto KVAL = [&](bc::reg r) LI_INLINE -> const any& {
			LI_ASSERT(f->proto->num_kval > (msize_t) r);
			return f->proto->kvals()[r];
		};
#endif
		const bc::insn* __restrict catchpad_i = nullptr;
		const auto* __restrict opcode_array = &f->proto->opcode_array[0];
		const auto* __restrict ip           = opcode_array;
		while (true) {
			const auto& __restrict insn = *ip++;
			auto [op, a, b, c]          = insn;
			switch (op) {
				UNOP_HANDLE(bc::TOBOOL)
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
					REG(a) = REG(b).type() == c;
					continue;
				}
				case bc::CTYX: {
					auto* cl = REG(c).as_vcl();
					bool  is_instance = false;
					if (auto o = REG(b); o.is_obj()) {
						for (auto it = o.as_obj()->cl; it; it = it->super) {
							if (it == cl) {
								is_instance = true;
								break;
							}
						}
					}
					REG(a) = is_instance;
					continue;
				}
				case bc::MOV: {
					REG(a) = REG(b);
					continue;
				}
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
					uint32_t it  = (uint32_t) iter.value;

					switch (target.type()) {
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
								iter.value = uint32_t(it + 1);

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
								iter.value = uint32_t(it + 1);

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
									iter.value = uint32_t(it + 1);

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
				case bc::TGET:
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
					} else if (tbl.is_str()) {
						if (!key.is_num() || key.as_num() < 0) [[unlikely]] {
							VM_RET(string::create(L, "indexing string with non-integer or negative key"), true);
						}
						auto i = size_t(key.as_num());
						auto v = tbl.as_str()->view();
						REG(a) = v.size() <= i ? any(nil) : any(number((uint8_t) v[i]));
					} else if (tbl.is_obj()) {
						any val = nil;
						if (key.is_str()) {
							val = tbl.as_obj()->get(key.as_str());
						}
						REG(a) = val;
					} else {
						VM_RET(string::create(L, "indexing non-table"), true);
					}
					continue;
				}
				case bc::TSET:
				case bc::TSETR: {
					auto tbl = REG(c);
					auto key = REG(a);
					auto val = REG(b);

					if (key == nil) [[unlikely]] {
						VM_RET(string::create(L, "indexing with null key"), true);
					}

					if (tbl.is_tbl()) {
						tbl.as_tbl()->set(L, key, val);
						L->gc.tick(L);
					} else if (tbl.is_arr()) {
						if (!key.is_num() || key.as_num() < 0) [[unlikely]] {
							VM_RET(string::create(L, "indexing array with non-integer or negative key"), true);
						}
						if (!tbl.as_arr()->set(L, msize_t(key.as_num()), val)) [[unlikely]] {
							VM_RET(string::create(L, "out-of-boundaries array access"), true);
						}
					} else if (tbl.is_obj()) {
						if (!key.is_str()) [[unlikely]] {
							VM_RET(string::create(L, "indexing class instance with non-string key"), true);
						}
						if (!tbl.as_obj()->set(L, key.as_str(), val)) {
							VM_RETHROW();
						}
					} else [[unlikely]] {
						VM_RET(string::create(L, "indexing non-table"), true);
					}
					continue;
				}

				case bc::STRIV: {
					L->gc.tick(L);
					REG(a) = object::create(L, any_t{insn.xmm()}.as_vcl());
					continue;
				}
				case bc::SSET: {
					auto tbl = REG(c);
					auto key = REG(a);
					auto val = REG(b);
					if (!key.is_str()) [[unlikely]] {
						VM_RET(string::create(L, "indexing class instance with non-string key"), true);
					}
					if (!tbl.as_obj()->set(L, key.as_str(), val)) {
						VM_RETHROW();
					}
					continue;
				}
				case bc::SGET: {
					auto tbl = REG(c);
					auto key = REG(b);
					any val = nil;
					if (key.is_str()) {
						val = tbl.as_obj()->get(key.as_str());
					}
					REG(a) = val;
					continue;
				}

				case bc::VACHK: {
					if (n_args < a) [[unlikely]] {
						VM_RET(any(any_t{insn.xmm()}), true);
					}
					continue;
				}
				case bc::VACNT: {
					REG(a) = number(n_args);
					continue;
				}
				case bc::VAGET: {
					auto idx = msize_t(REG(b).as_num());
					REG(a)   = n_args > idx ? args[-(int32_t)idx] : nil;
					continue;
				}

				case bc::ANEW: {
					L->gc.tick(L);
					REG(a) = any{array::create(L, b)};
					continue;
				}
				case bc::TNEW: {
					L->gc.tick(L);
					REG(a) = any{table::create(L, b)};
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
				case bc::SETEH: {
					if (a) {
						catchpad_i = ip + a;
					} else {
						catchpad_i = nullptr;
					}
					continue;
				}
				case bc::SETEX: {
					L->last_ex = REG(a);
					continue;
				}
				case bc::GETEX: {
					REG(a) = L->last_ex;
					continue;
				}
				case bc::CALL: {
					call_frame cf{.caller_pc = msize_t(ip - 1 - opcode_array), .stack_pos = msize_t(locals_begin - L->stack)};
					auto       argspace = L->stack_top - 3;
				
					L->push_stack(any(std::in_place, li::bit_cast<uint64_t>(cf)));
					any result = vm_invoke(L, argspace, b);
					if (result.is_exc()) [[unlikely]] {
						VM_RETHROW();
					}
					REG(a)       = result;
					L->stack_top = reset_point;
					continue;
				}
				case bc::PUSHR:
					L->push_stack(REG(a));
					continue;
				case bc::PUSHI:
					L->push_stack(any_t{insn.xmm()});
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