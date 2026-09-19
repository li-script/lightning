#include <bit>
#include <cmath>
#include <lang/parser.hpp>
#include <lib/std.hpp>
#include <util/user.hpp>
#include <vm/array.hpp>
#include <vm/function.hpp>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/shared.hpp>
#include <vm/string.hpp>
#include <vm/types.hpp>

namespace li::lib {
	// Registers the debug library.
	//
	void register_debug(vm* L) {
		util::export_as(L, "debug.isdebug", any(bool(LI_DEBUG)));
		util::export_as(L, "debug.sizeof", [](vm* L, any* args, slot_t n) {
			if (n && args->is_obj()) {
				return L->ok((number) args->as_obj()->cl->object_length);
			}
			if (n && args->is_vcl()) {
				return L->ok((number) args->as_vcl()->object_length);
			}
			return L->ok(nil);
		});
		util::export_as(L, "debug.offsetof", [](vm* L, any* args, slot_t n) {
			if (n == 2 && args[-1].is_str()) {
				any x = *args;
				if (x.is_obj())
					x = x.as_obj()->cl;
				if (x.is_vcl()) {
					shared::recursive_guard guard(L, x.as_vcl());
					for (auto& f : x.as_vcl()->fields()) {
						if (string_value_equals(f.key, args[-1].as_str())) {
							return L->ok(number(f.value.offset));
						}
					}
				}
			}
			return L->ok(nil);
		});
		util::export_as(L, "debug.stacktrace", [](vm* L, any* args, slot_t n) {
			vm_stack_guard _g{L, args};

			auto result = array::create(L, 0, 10);
			auto cstr   = string::create(L, "C");
			auto lstr   = string::create(L, "line");
			auto fstr   = string::create(L, "func");
			auto fail   = [&]() -> any_t {
				rc::release(L, result);
				rc::release(L, cstr);
				rc::release(L, lstr);
				rc::release(L, fstr);
				return exception_marker;
			};

			call_frame frame = L->last_vm_caller;
			while (frame.stack_pos >= FRAME_SIZE) {
				auto& target = L->stack[frame.stack_pos + FRAME_TARGET];

				if (frame.multiplexed_by_c()) {
					auto tbl = table::create(L, 1);
					if (!tbl->set(L, (any) fstr, (any) cstr) || !result->push(L, tbl)) {
						rc::release(L, tbl);
						return fail();
					}
					rc::release(L, tbl);
				}

				auto tbl = table::create(L, 2);
				if (target.is_fn() && target.as_fn()->is_virtual()) {
					if (!tbl->set(L, (any) lstr, any(number(target.as_fn()->proto->lookup_line(frame.caller_pc & ~FRAME_C_FLAG))))) {
						rc::release(L, tbl);
						return fail();
					}
				}
				if (!tbl->set(L, (any) fstr, any(target)) || !result->push(L, tbl)) {
					rc::release(L, tbl);
					return fail();
				}
				rc::release(L, tbl);

				auto ref = L->stack[frame.stack_pos + FRAME_CALLER];
				frame    = li::bit_cast<call_frame>(ref.value);
			}
			rc::release(L, cstr);
			rc::release(L, lstr);
			rc::release(L, fstr);
			return L->take(result);
		});
		util::export_as(L, "debug.traceback", [](vm* L, any* args, slot_t n) -> any_t {
			if (n > 1)
				return L->error("traceback expects zero or one argument");
			any    error  = n ? args[0] : L->last_ex;
			array* result = L->copy_exception_trace(error);
			if (!result)
				return exception_marker;
			return L->take(result);
		});
		util::export_as(L, "debug.getuval", [](vm* L, any* args, slot_t n) {
			if (n != 2) {
				return L->error("expected 2 arguments.");
			}
			auto f = args[0];
			if (!f.is_fn()) {
				return L->error("expected function.");
			}

			auto i = args[-1];
			if (!i.is_num() || i.as_num() < 0) {
				return L->error("expected positive index.");
			}
			size_t idx = size_t(i.as_num());

			shared::recursive_guard guard(L, f.as_fn());
			if (f.as_fn()->num_uval > idx) {
				return L->ok(f.as_fn()->uvals()[idx]);
			} else {
				return L->ok(nil);
			}
		});
		util::export_as(L, "debug.setuval", [](vm* L, any* args, slot_t n) -> any_t {
			if (n != 3) {
				return L->error("expected 3 arguments.");
			}
			auto f = args[0];
			if (!f.is_fn()) {
				return L->error("expected function.");
			}

			auto i = args[-1];
			if (!i.is_num() || i.as_num() < 0) {
				return L->error("expected positive index.");
			}
			size_t idx = size_t(i.as_num());

			auto      u       = args[-2];
			function* closure = f.as_fn();
			if (closure->num_uval <= idx)
				return L->ok(false);

			shared::prepared_value prepared = shared::prepare_store(L, closure, u);
			if (!prepared.ok)
				return exception_marker;
			bool replaced = false;
			{
				shared::recursive_guard guard(L, closure);
				replaced = rc::try_replace(L, closure->uvals()[idx], prepared.value);
			}
			shared::finish_store(L, prepared);
			if (!replaced)
				return exception_marker;
			return L->ok(true);
		});
		util::export_as(L, "debug.dump", [](vm* L, any* args, slot_t n) {
			if (n != 1 || !args->is_fn() || !args->as_fn()->is_virtual()) {
				return L->error("dump expects a single vfunction");
			}
			auto f = args->as_fn();
			f->print_bc();
			return L->ok();
		});

		util::export_as(L, "debug.live_objects", [](vm* L, any* args, slot_t n) { return L->ok((number) L->gc.live_objects); });
		// Retain/release counters are scoped to the calling thread, not the VM.
		util::export_as(L, "debug.retain_count", [](vm* L, any* args, slot_t n) { return L->ok((number) rc::counts().retains); });
		util::export_as(L, "debug.release_count", [](vm* L, any* args, slot_t n) { return L->ok((number) rc::counts().releases); });
	}
};
