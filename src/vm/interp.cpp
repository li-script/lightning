#include <cmath>
#include <lang/operator.hpp>
#include <utility>
#include <vm/array.hpp>
#include <vm/bc.hpp>
#include <vm/function.hpp>
#include <vm/iterator.hpp>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/runtime.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <vm/tier.hpp>
#include <vm/traits.hpp>

namespace li {
	struct vm_exception_handler_scope {
		vm*         L;
		const void* previous_exception;
		const void* previous_cleanup;
		uint32_t    previous_cleanup_depth;

		explicit vm_exception_handler_scope(vm* state, const void* exception = nullptr, const void* cleanup = nullptr) noexcept
			 : L(state),
				previous_exception(std::exchange(state->exception_handler, exception)),
				previous_cleanup(std::exchange(state->cleanup_handler, cleanup)),
				previous_cleanup_depth(state->cleanup_depth) {}

		~vm_exception_handler_scope() {
			L->exception_handler = previous_exception;
			L->cleanup_handler   = previous_cleanup;
			L->cleanup_depth     = previous_cleanup_depth;
		}

		vm_exception_handler_scope(const vm_exception_handler_scope&)            = delete;
		vm_exception_handler_scope& operator=(const vm_exception_handler_scope&) = delete;
	};

	// VM helpers.
	//
#define VM_RETHROW()                                                                                   \
	{                                                                                                   \
		if (!L->forced_unwind_requested() && ip > opcode_array && ip <= opcode_array + f->proto->length) \
			L->capture_exception_trace(f, uint32_t(ip - 1 - opcode_array), locals_begin);                 \
		if (L->forced_unwind_requested()) {                                                              \
			if (cleanup_i) {                                                                              \
				ip = cleanup_i;                                                                            \
				L->truncate_stack(reset_point);                                                            \
				continue;                                                                                  \
			}                                                                                             \
		} else if (catchpad_i) {                                                                         \
			ip = catchpad_i;                                                                              \
			L->truncate_stack(reset_point);                                                               \
			continue;                                                                                     \
		}                                                                                                \
		L->truncate_stack(locals_begin);                                                                 \
		return exception_marker;                                                                         \
	}
#define VM_RETURN(value)                         \
	{                                             \
		any vm_return_value__ = (value);           \
		if (!rc::try_retain(L, vm_return_value__)) \
			VM_RETHROW();                           \
		L->truncate_stack(locals_begin);           \
		return L->take(vm_return_value__);         \
	}

#define UNOP_HANDLE(K)                    \
	case K: {                              \
		auto r = apply_unary(L, REG(b), K); \
		if (r.is_exc()) [[unlikely]]        \
			VM_RETHROW();                    \
		rc::replace_adopt(L, REG(a), r);    \
		continue;                           \
	}
#define BINOP_HANDLE(K)                            \
	case K: {                                       \
		auto r = apply_binary(L, REG(b), REG(c), K); \
		if (r.is_exc()) [[unlikely]]                 \
			VM_RETHROW();                             \
		rc::replace_adopt(L, REG(a), r);             \
		continue;                                    \
	}

#if !LI_DEBUG
	#define REG(...)  locals_begin[(__VA_ARGS__)]
	#define UVAL(...) f->uvals()[(__VA_ARGS__)]
	#define KVAL(...) f->proto->kvals()[(__VA_ARGS__)]
#endif
	namespace {
		// Keep the large, instrumented interpreter frame behind vm_invoke's
		// native-stack check. In particular, do not inline it back across that
		// check: ASan reserves this frame before executing the function body.
		LI_NOINLINE any_t vm_interpret(vm* L, any* args, slot_t n_args, function* f, uint32_t bytecode_pc, uint32_t exception_handler_pc,
			 uint32_t cleanup_handler_pc, bool initialize_locals) {
			any* const __restrict locals_begin = args + FRAME_SIZE + 1;

			// A fresh entry allocates and initializes its locals. A resume entry
			// consumes the already-owning frame materialized by generated code.
			//
			msize_t num_locals = f->proto->num_locals;
			any*    reset_point;
			if (initialize_locals)
				reset_point = L->alloc_stack(num_locals) + num_locals;
			else
				reset_point = locals_begin + num_locals;

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
			const auto* __restrict opcode_array   = &f->proto->opcode_array[0];
			const bc::insn* __restrict catchpad_i = exception_handler_pc == no_interpreter_handler ? nullptr : opcode_array + exception_handler_pc;
			const bc::insn* __restrict cleanup_i  = cleanup_handler_pc == no_interpreter_handler ? nullptr : opcode_array + cleanup_handler_pc;
			const auto* __restrict ip             = opcode_array + bytecode_pc;
			vm_exception_handler_scope handler_scope{L, catchpad_i, cleanup_i};
			auto                       try_osr = [&](const bc::insn* target) -> std::optional<any_t> {
				any  result;
				auto target_pc = uint32_t(target - opcode_array);
				if (tier::on_backedge(L, f, args, n_args, target_pc, &result))
					return result;
				return std::nullopt;
			};
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
						rc::replace_adopt(L, REG(a), any(string::concat(L, &REG(a), b)));
						continue;
					}
					case bc::CTY: {
						rc::replace(L, REG(a), any(REG(b).type() == c));
						continue;
					}
					case bc::CTYX: {
						rc::replace(L, REG(a), any(bool(class_matches(REG(b), REG(c).as_vcl()->identity))));
						continue;
					}
					case bc::CTYID: {
						rc::replace(L, REG(a), any(bool(class_matches(REG(b), f->proto->return_class_identity))));
						continue;
					}
					case bc::MOV: {
						rc::replace_frame(L, REG(a), REG(b));
						continue;
					}
					case bc::RET:
						if (REG(a).is_exc()) [[unlikely]]
							VM_RETHROW();
						VM_RETURN(REG(a));
					case bc::JNS:
						if (REG(b).coerce_bool())
							continue;
						if (a < 0) {
							if (auto result = try_osr(ip + a))
								return *result;
						}
						ip += a;
						continue;
					case bc::JS:
						if (!REG(b).coerce_bool())
							continue;
						if (a < 0) {
							if (auto result = try_osr(ip + a))
								return *result;
						}
						ip += a;
						continue;
					case bc::JMP:
						if (a < 0) {
							if (auto result = try_osr(ip + a))
								return *result;
						}
						ip += a;
						continue;
					case bc::ITER: {
						any result = iterator_step(L, REG(c), REG(b), REG(b + 1), REG(b + 2));
						if (result.is_exc()) [[unlikely]]
							VM_RETHROW();
						if (!result.as_bool()) {
							if (a < 0) {
								if (auto osr_result = try_osr(ip + a))
									return *osr_result;
							}
							ip += a;
						}
						continue;
					}
					case bc::KIMM: {
						rc::replace_frame(L, REG(a), any(std::in_place, insn.xmm()));
						continue;
					}
					case bc::UGET: {
						if (!f->shared) {
							rc::replace_frame(L, REG(a), UVAL(b));
						} else {
							any result = runtime::capture_get(L, f, b);
							rc::replace_adopt(L, REG(a), result);
						}
						continue;
					}
					case bc::USET: {
						if (!f->shared) {
							if (!rc::try_replace(L, UVAL(a), REG(b))) [[unlikely]]
								VM_RETHROW();
						} else {
							any result = runtime::capture_set(L, f, a, REG(b));
							if (result.is_exc()) [[unlikely]]
								VM_RETHROW();
							rc::release(L, result);
						}
						continue;
					}
					case bc::TGET: {
						any result = runtime::field_get(L, REG(c), REG(b));
						if (result.is_exc()) [[unlikely]] {
							VM_RETHROW();
						}
						rc::replace_adopt(L, REG(a), result);
						continue;
					}
					case bc::TGETR: {
						any result = runtime::field_get_raw(L, REG(c), REG(b));
						if (result.is_exc()) [[unlikely]] {
							VM_RETHROW();
						}
						rc::replace_adopt(L, REG(a), result);
						continue;
					}
					case bc::TSET: {
						any result = runtime::field_set(L, REG(c), REG(a), REG(b));
						if (result.is_exc()) [[unlikely]] {
							VM_RETHROW();
						}
						rc::release(L, result);
						continue;
					}
					case bc::TSETR: {
						any result = runtime::field_set_raw(L, REG(c), REG(a), REG(b));
						if (result.is_exc()) [[unlikely]] {
							VM_RETHROW();
						}
						rc::release(L, result);
						continue;
					}

					case bc::STRIV: {
						auto self = REG(FRAME_SELF);
						if (!self.is_vcl()) [[unlikely]] {
							L->error("class constructor invoked without class self");
							VM_RETHROW();
						}
						rc::replace_adopt(L, REG(a), any(object::create(L, self.as_vcl())));
						continue;
					}
					case bc::SSET: {
						any result = runtime::field_set_raw(L, REG(c), REG(a), REG(b));
						if (result.is_exc()) [[unlikely]] {
							VM_RETHROW();
						}
						rc::release(L, result);
						continue;
					}
					case bc::SGET: {
						any result = runtime::field_get_raw(L, REG(c), REG(b));
						if (result.is_exc()) [[unlikely]] {
							VM_RETHROW();
						}
						rc::replace_adopt(L, REG(a), result);
						continue;
					}

					case bc::VACHK: {
						if (n_args < a) [[unlikely]] {
							L->error(any(any_t{insn.xmm()}));
							VM_RETHROW();
						}
						continue;
					}
					case bc::VACNT: {
						rc::replace(L, REG(a), any(number(n_args)));
						continue;
					}
					case bc::VAGET: {
						any    value = nil;
						auto   key   = REG(b);
						number index = key.is_num() ? key.as_num() : -1;
						if (std::isfinite(index) && index >= 0 && index == std::trunc(index) && index < number(n_args)) {
							value = args[-slot_t(index)];
						}
						rc::replace_frame(L, REG(a), value);
						continue;
					}

					case bc::ANEW: {
						rc::replace_adopt(L, REG(a), any(array::create(L, b)));
						continue;
					}
					case bc::TNEW: {
						rc::replace_adopt(L, REG(a), any(table::create(L, b)));
						continue;
					}
					case bc::FDUP: {
						auto fn = KVAL(b);
						LI_ASSERT(fn.is_fn());

						function* r = fn.as_fn()->duplicate(L);
						for (msize_t i = 0; i != r->num_uval; i++) {
							if (!r->shared) {
								if (!rc::try_replace(L, r->uvals()[i], REG(c + i))) [[unlikely]] {
									rc::release(L, r);
									VM_RETHROW();
								}
							} else {
								any result = runtime::capture_set(L, r, int32_t(i), REG(c + i));
								if (result.is_exc()) [[unlikely]] {
									rc::release(L, r);
									VM_RETHROW();
								}
								rc::release(L, result);
							}
						}
						rc::replace_adopt(L, REG(a), any(r));
						continue;
					}
					case bc::SETEH: {
						catchpad_i           = a ? ip + a : nullptr;
						cleanup_i            = c ? ip + c : nullptr;
						L->exception_handler = catchpad_i;
						L->cleanup_handler   = cleanup_i;
						continue;
					}
					case bc::SETEX: {
						if (L->set_exception(REG(a), f, uint32_t(ip - 1 - opcode_array), locals_begin).is_exc()) [[unlikely]]
							VM_RETHROW();
						continue;
					}
					case bc::GETEX: {
						rc::replace_frame(L, REG(a), L->last_ex);
						continue;
					}
					case bc::CALL: {
						if (!L->vm_stack_headroom_available(1)) [[unlikely]] {
							L->truncate_stack(reset_point);
							L->vm_stack_exhausted();
							VM_RETHROW();
						}
						call_frame cf{.caller_pc = msize_t(ip - 1 - opcode_array), .stack_pos = msize_t(locals_begin - L->stack)};
						auto       argspace = L->stack_top - 3;

						L->push_stack(any(std::in_place, li::bit_cast<uint64_t>(cf)));
						any result = vm_invoke(L, argspace, b);
						L->truncate_stack(reset_point);
						if (result.is_exc()) [[unlikely]] {
							VM_RETHROW();
						}
						rc::replace_adopt(L, REG(a), result);
						continue;
					}
					case bc::PUSHR:
						if (!L->vm_stack_headroom_available(1)) [[unlikely]] {
							L->truncate_stack(reset_point);
							L->vm_stack_exhausted();
							VM_RETHROW();
						}
						L->push_stack(REG(a));
						continue;
					case bc::PUSHI:
						if (!L->vm_stack_headroom_available(1)) [[unlikely]] {
							L->truncate_stack(reset_point);
							L->vm_stack_exhausted();
							VM_RETHROW();
						}
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
	}

	any_t LI_CC vm_interpret_resume(vm* L, any* args, slot_t n_args, uint32_t bytecode_pc, uint32_t exception_handler_pc, uint32_t cleanup_handler_pc) {
		if (!L)
			return exception_marker;
		if (!args || n_args < 0)
			return L->error("invalid interpreter resume frame");
		any* const locals_begin = args + FRAME_SIZE + 1;
		if (locals_begin < L->stack || locals_begin > L->stack_top)
			return L->error("invalid interpreter resume frame");
		const slot_t frame_prefix = slot_t(locals_begin - L->stack);
		if (frame_prefix < FRAME_SIZE || n_args > frame_prefix - FRAME_SIZE)
			return L->error("invalid interpreter resume argument count");
		any& target = locals_begin[FRAME_TARGET];
		if (!target.is_fn() || !target.as_fn()->is_virtual())
			return L->error("interpreter resume target is not a virtual function");
		function*       f     = target.as_fn();
		function_proto* proto = f->proto;
		if (proto->num_locals > msize_t(L->stack_limit - locals_begin) || locals_begin + proto->num_locals != L->stack_top)
			return L->error("interpreter resume frame does not own exactly its locals");
		auto fail_resume = [&](const char* message) {
			any result = L->error("%s", message);
			L->truncate_stack(locals_begin);
			return result;
		};
		auto valid_pc = [proto](uint32_t pc) { return pc == no_interpreter_handler || pc < proto->length; };
		if (bytecode_pc >= proto->length || !valid_pc(exception_handler_pc) || !valid_pc(cleanup_handler_pc))
			return fail_resume("invalid interpreter resume bytecode offset");
		if (!L->native_stack_headroom_available()) [[unlikely]]
			return fail_resume("native stack exhausted while resuming script frame");
		return vm_interpret(L, args, n_args, f, bytecode_pc, exception_handler_pc, cleanup_handler_pc, false);
	}

	any_t LI_CC vm_invoke(vm* L, any* args, slot_t n_args) {
		LI_ASSERT(&args[2] == &L->stack_top[FRAME_TARGET]);
		any* const __restrict locals_begin = args + FRAME_SIZE + 1;

		// Validate function. A class call keeps the class in self while replacing
		// the target with its constructor; both frame slots remain owning.
		//
		auto& vf = locals_begin[FRAME_TARGET];
		if (vf.is_vcl()) {
			vclass*   cl   = vf.as_vcl();
			function* ctor = pin_class_ctor(L, cl);
			rc::replace_frame(L, locals_begin[FRAME_SELF], vf);
			rc::replace_adopt(L, vf, any(ctor));
		} else if (!vf.is_fn()) {
			any original_target = vf;
			if (function* callable = pin_trait(L, original_target, trait::call)) {
				rc::replace_frame(L, locals_begin[FRAME_SELF], original_target);
				rc::replace_adopt(L, vf, any(callable));
			}
		}
		if (!vf.is_fn()) [[unlikely]]
			return L->error("invoking non-function");

		function* f = vf.as_fn();
		if (f->is_virtual() && !L->native_stack_headroom_available()) [[unlikely]]
			return L->error("native stack exhausted while entering script frame");

		nfunc_t invoke = f->invoke;
		if (f->is_virtual()) {
			if (tier::is_enabled(f)) {
				if (!f->proto->load_jfunc()) {
					if (L->jit_policy.execution == tier::mode::required) {
						if (auto error = tier::compile_required(L, f))
							return L->error("JIT compilation failed (unsupported opcode or backend operation: %s).", error->c_str());
					} else {
						tier::on_call(L, f);
					}
				}
				if (f->proto->load_jfunc())
					invoke = &jit_dispatch;
			}
			if (invoke != &jit_dispatch || !tier::is_enabled(f)) {
				if (!L->vm_stack_headroom_available(f->proto->num_locals)) [[unlikely]]
					return L->vm_stack_exhausted();
				return vm_interpret(L, args, n_args, f, 0, no_interpreter_handler, no_interpreter_handler, true);
			}
		}

		// Class/trait target replacement above adopts the final function into the
		// published FRAME_TARGET slot. That owning slot is not written again until
		// the caller truncates the completed frame, so invocation may borrow vf.
		return invoke(L, args, n_args);
	}
};