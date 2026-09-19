#include <lib/std.hpp>
#include <util/user.hpp>
#include <vm/string.hpp>
#include <vm/traits.hpp>

namespace li::lib {
	namespace {
		trait checked_trait_name(vm* L, any_t value) {
			if (!value.is_str()) {
				L->error("trait name must be a string");
				return trait::none;
			}
			trait which = resolve_trait_name(value.as_str()->view());
			if (which == trait::none)
				L->error("unknown trait '%.*s'", int(value.as_str()->length), value.as_str()->data);
			return which;
		}

		any_t LI_CC traits_get(vm* L, any* args, slot_t count) {
			if (count != 2)
				return L->error("traits.get expects a target and trait name");
			trait which = checked_trait_name(L, args[-1]);
			if (which == trait::none)
				return exception_marker;
			return get_trait(L, args[0], which);
		}

		any_t LI_CC traits_set(vm* L, any* args, slot_t count) {
			if (count != 3)
				return L->error("traits.set expects a target, trait name, and value");
			trait which = checked_trait_name(L, args[-1]);
			if (which == trait::none)
				return exception_marker;
			return set_trait(L, args[0], which, args[-2]);
		}
	}

	void register_traits(vm* L) {
		util::export_as(L, "traits.get", &traits_get);
		util::export_as(L, "traits.set", &traits_set);
	}
}
