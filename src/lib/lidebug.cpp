#include <lib/std.hpp>
#include <util/user.hpp>
#include <vm/function.hpp>
#include <vm/array.hpp>
#include <lang/parser.hpp>
#include <cmath>
#include <bit>

namespace li::lib {
	// Registers the debug library.
	//
	void register_debug(vm* L) {
		util::export_as(L, "debug.stacktrace", [](vm* L, any* args, slot_t n) {
			vm_guard _g{L, args};

			auto result = array::create(L, 0, 10);
			auto cstr   = string::create(L, "C");
			auto lstr   = string::create(L, "line");
			auto fstr   = string::create(L, "func");

			call_frame frame = L->last_vm_caller;
			while (frame.stack_pos >= FRAME_SIZE) {
				auto& target = L->stack[frame.stack_pos + FRAME_TARGET];

				if (frame.multiplexed_by_c()) {
					auto tbl = table::create(L, 1);
					tbl->set(L, fstr, cstr);
					result->push(L, tbl);
				}

				auto tbl = table::create(L, 2);
				if (target.is_fn() && target.as_fn()->is_virtual()) {
					tbl->set(L, lstr, any(number(target.as_fn()->proto->lookup_line(frame.caller_pc & ~FRAME_C_FLAG))));
				}
				tbl->set(L, fstr, any(target));
				result->push(L, tbl);

				auto ref = L->stack[frame.stack_pos + FRAME_CALLER];
				if (ref.is_opq()) {
					frame = bit_cast<call_frame>(ref.as_opq());
				} else {
					break;
				}
			}
			return L->ok(result);
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

			if (f.as_fn()->num_uval > idx) {
				return L->ok(f.as_fn()->uvals()[idx]);
			} else {
				return L->ok(nil);
			}
		});
		util::export_as(L, "debug.setuval", [](vm* L, any* args, slot_t n) {
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

			auto u = args[-2];
			if (f.as_fn()->num_uval > idx) {
				f.as_fn()->uvals()[idx] = u;
				return L->ok(true);
			} else {
				return L->ok(false);
			}
		});
		util::export_as(L, "debug.gc", [](vm* L, any* args, slot_t n) {
			L->gc.collect(L);
			return L->ok();
		});
		util::export_as(L, "debug.dump", [](vm* L, any* args, slot_t n) {
			if (n != 1 || !args->is_fn() || !args->as_fn()->is_virtual()) {
				return L->error("dump expects a single vfunction");
			}
			auto f = args->as_fn();
			f->print_bc();
			return L->ok();
		});
	}
};
