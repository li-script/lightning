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
		util::export_as(L, "debug.isdebug", any(bool(LI_DEBUG)));
		util::export_as(L, "debug.stacktrace", [](vm* L, any* args, slot_t n) {
			vm_stack_guard _g{L, args};

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
				frame    = li::bit_cast<call_frame>(ref.value);
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
		util::export_as(L, "debug.dump", [](vm* L, any* args, slot_t n) {
			if (n != 1 || !args->is_fn() || !args->as_fn()->is_virtual()) {
				return L->error("dump expects a single vfunction");
			}
			auto f = args->as_fn();
			f->print_bc();
			return L->ok();
		});
		
		util::export_as(L, "gc.collect", [](vm* L, any* args, slot_t n) {
			L->gc.collect(L);
			return L->ok();
		});
		util::export_as(L, "gc.tick", [](vm* L, any* args, slot_t n) {
			L->gc.tick(L);
			return L->ok();
		});
		util::export_as(L, "gc.used_memory", [](vm* L, any* args, slot_t n) {
			number result = 0;
			L->gc.for_each([&](gc::page* p, bool) {
				result += p->num_pages * ((4096.0) / (1024.0 * 1024.0));
				return false;
			});
			return L->ok(result);
		});
		util::export_as(L, "gc.debt", [](vm* L, any* args, slot_t n) {
			return L->ok(gc::chunk_size * (number) L->gc.debt);
		});
		util::export_as(L, "gc.greedy", [](vm* L, any* args, slot_t n) {
			if (n >= 1) {
				L->gc.greedy = args->coerce_bool();
				L->gc.collect(L);
			}
			return L->ok(L->gc.greedy);
		});
		util::export_as(L, "gc.interval", [](vm* L, any* args, slot_t n) {
			if (n >= 1) {
				if (!args->is_num())
					return L->error("expected one number");
				L->gc.interval = (msize_t) args->as_num();
				L->gc.collect(L);
			}
			return L->ok((number)L->gc.interval);
		});
		util::export_as(L, "gc.max_debt", [](vm* L, any* args, slot_t n) {
			if (n >= 1) {
				if (!args->is_num())
					return L->error("expected one number");
				L->gc.max_debt = (msize_t) args->as_num() / gc::chunk_size;
				L->gc.collect(L);
			}
			return L->ok(gc::chunk_size * (number) L->gc.max_debt);
		});
		util::export_as(L, "gc.min_debt", [](vm* L, any* args, slot_t n) {
			if (n >= 1) {
				if (!args->is_num())
					return L->error("expected one number");
				L->gc.min_debt = (msize_t) args->as_num() / gc::chunk_size;
				L->gc.collect(L);
			}
			return L->ok(gc::chunk_size * (number) L->gc.min_debt);
		});
		util::export_as(L, "gc.counter", [](vm* L, any* args, slot_t n) {
			return L->ok((number)L->gc.collect_counter);
		});
	}
};
