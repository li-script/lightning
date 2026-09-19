#include <cmath>
#include <lib/std.hpp>
#include <util/user.hpp>
#include <vm/atomic.hpp>
#include <vm/shared.hpp>

namespace li::lib {
	namespace {
		any_t LI_CC shared_is_shared(vm* L, any* args, slot_t count) {
			if (count != 1)
				return L->error("shared.is_shared expects one value");
			return L->ok(shared::is_shared(args[0]));
		}

		any_t LI_CC shared_atomic_add(vm* L, any* args, slot_t count) {
			if (count != 3 || !args[0].is_gc() || !shared::is_shared(args[0]) || !args[-2].is_num())
				return L->error("shared.atomic_add expects a shared value, field key, and numeric delta");
			return shared::atomic_add(L, args[0].as_gc(), args[-1], args[-2].as_num());
		}

		any_t LI_CC atomic_field_store(vm* L, any* args, slot_t count) {
			if (count != 3)
				return L->error("builtin.atomic_field_store expects an object, field key, and numeric value");
			return shared::atomic_field_store(L, args[0], args[-1], args[-2]);
		}

		any_t LI_CC atomic_field_update(vm* L, any* args, slot_t count) {
			if (count != 4 || !args[-2].is_num() || !args[-3].is_num())
				return L->error("builtin.atomic_field_update expects an object, field key, operation, and numeric operand");
			const number operation = args[-2].as_num();
			if (!std::isfinite(operation) || operation != std::trunc(operation) || operation < static_cast<number>(shared::numeric_operation::set) ||
				 operation > static_cast<number>(shared::numeric_operation::mod))
				return L->error("builtin.atomic_field_update received an invalid operation");
			return shared::atomic_field_update(L, args[0], args[-1], static_cast<shared::numeric_operation>(uint8_t(operation)), args[-3].as_num());
		}

		util::native_function atomic_field_store_builtin = {
			 func_attr_sideeffect | func_attr_c_takes_vm,
			 "builtin.atomic_field_store",
			 &atomic_field_store,
		};
		util::native_function atomic_field_update_builtin = {
			 func_attr_sideeffect | func_attr_c_takes_vm,
			 "builtin.atomic_field_update",
			 &atomic_field_update,
		};
	}

	void register_shared(vm* L) {
		atomic::detail::make_shared.export_into(L);
		util::export_as(L, "shared.is_shared", &shared_is_shared);
		atomic::detail::lock_shared.export_into(L);
		atomic::detail::unlock_shared.export_into(L);
		util::export_as(L, "shared.atomic_add", &shared_atomic_add);
		atomic_field_store_builtin.export_into(L);
		atomic_field_update_builtin.export_into(L);
	}
}
