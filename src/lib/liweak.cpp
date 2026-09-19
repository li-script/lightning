#include <lib/std.hpp>
#include <util/user.hpp>
#include <vm/weak.hpp>

namespace li::lib {
	void register_weak(vm* L) {
		util::export_as(L, "weak.create", [](vm* L, any* args, slot_t n) -> any_t {
			if (n != 1 || !args->is_gc())
				return L->error("weak.create expected reference");
			return L->take(weak::create(L, args->as_gc()));
		});
		util::export_as(L, "weak.lock", [](vm* L, any* args, slot_t n) -> any_t {
			if (n != 1 || !args->is_weak())
				return L->error("weak.lock expected weak reference");
			return args->as_weak()->lock();
		});
		util::export_as(L, "weak.expired", [](vm* L, any* args, slot_t n) -> any_t {
			if (n != 1 || !args->is_weak())
				return L->error("weak.expired expected weak reference");
			return L->ok(args->as_weak()->expired());
		});
	}
}
