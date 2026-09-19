#include <array>
#include <cmath>
#include <limits>
#include <vm/array.hpp>
#include <vm/function.hpp>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/runtime.hpp>
#include <vm/shared.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <vm/traits.hpp>
#include <vm/typed_array.hpp>

namespace li::runtime {
	static any_t invoke_property(vm* L, function* method, any_t self, std::span<const any> args = {}) {
		if (!L->vm_stack_headroom_available(args.size() + FRAME_SIZE)) [[unlikely]] {
			rc::release(L, method);
			return L->vm_stack_exhausted();
		}
		for (size_t index = args.size(); index != 0; --index)
			L->push_stack(args[index - 1]);
		any result = L->call(slot_t(args.size()), any(method), self);
		rc::release(L, method);
		return result;
	}

	template<typename T>
	static bool checked_index(any_t key, T& index) {
		if (!key.is_num()) [[unlikely]] {
			return false;
		}

		const number value = key.as_num();
		if (!std::isfinite(value) || value < 0 || value != std::trunc(value)) [[unlikely]] {
			return false;
		}
		if (value >= std::ldexp(1.0, std::numeric_limits<T>::digits)) [[unlikely]] {
			return false;
		}

		index = static_cast<T>(value);
		return true;
	}

	any_t LI_CC field_set_raw(vm* L, any_t target, any_t key, any_t value) {
		if (key == nil) [[unlikely]] {
			return L->error("indexing with null key");
		}
		if (target.is_obj() && target.as_obj()->shared && key.is_str() && shared::is_atomic_field(target.as_obj(), key.as_str()))
			return shared::atomic_field_store(L, target, key, value);

		if (target.is_tbl()) {
			if (!target.as_tbl()->set(L, key, value)) [[unlikely]] {
				return exception_marker;
			}
		} else if (target.is_arr()) {
			msize_t index = 0;
			if (!checked_index(key, index)) [[unlikely]] {
				return L->error("indexing array with non-integer or negative key");
			}
			if (index >= target.as_arr()->size()) [[unlikely]] {
				return L->error("out-of-boundaries array access");
			}
			if (!target.as_arr()->set(L, index, value)) [[unlikely]] {
				return exception_marker;
			}
		} else if (target.is_tarr()) {
			msize_t index = 0;
			if (!checked_index(key, index)) [[unlikely]] {
				return L->error("indexing typed array with non-integer or negative key");
			}
			return target.as_tarr()->set(L, index, value);
		} else if (target.is_obj()) {
			if (!key.is_str()) [[unlikely]] {
				return L->error("indexing class instance with non-string key");
			}
			if (!target.as_obj()->set(L, key.as_str(), value)) [[unlikely]] {
				return exception_marker;
			}
		} else [[unlikely]] {
			return L->error("indexing non-table");
		}
		return L->ok();
	}

	any_t LI_CC field_set(vm* L, any_t target, any_t key, any_t value) {
		if (key == nil) [[unlikely]] {
			return L->error("indexing with null key");
		}
		if (trait_flag_enabled(target, trait::freeze)) [[unlikely]] {
			return L->error("modifying frozen value");
		}
		if (target.is_obj() && target.as_obj()->shared && key.is_str() && shared::is_atomic_field(target.as_obj(), key.as_str()))
			return shared::atomic_field_store(L, target, key, value);
		if (target.is_obj() && key.is_str()) {
			object* instance = target.as_obj();
			if (function* setter = pin_class_property(L, instance->cl, key.as_str(), property_access::set)) {
				std::array<any, 1> args = {any(value)};
				return invoke_property(L, setter, target, args);
			}
			if (class_has_property(instance->cl, key.as_str()))
				return L->error("assigning read-only property");
		}
		if (has_trait(target, trait::set)) {
			std::array<any, 2> args = {any(key), any(value)};
			return invoke_trait(L, trait::set, target, args);
		}
		return field_set_raw(L, target, key, value);
	}

	any_t LI_CC field_delete(vm* L, any_t target, any_t key) {
		if (key == nil) [[unlikely]] {
			return L->error("deleting with null key");
		}
		if (!target.is_tbl()) [[unlikely]] {
			return L->error("deleting field from non-table");
		}
		if (trait_flag_enabled(target, trait::freeze)) [[unlikely]] {
			return L->error("modifying frozen value");
		}
		return L->ok(target.as_tbl()->erase(L, key));
	}

	any_t LI_CC field_get_raw(vm* L, any_t target, any_t key) {
		if (key == nil) [[unlikely]] {
			return L->error("indexing with null key");
		}

		if (target.is_tbl()) {
			shared::recursive_guard guard(L, target.as_tbl());
			return L->ok(target.as_tbl()->get(L, key));
		}
		if (target.is_arr()) {
			msize_t index = 0;
			if (!checked_index(key, index)) [[unlikely]] {
				return L->error("indexing array with non-integer or negative key");
			}
			shared::recursive_guard guard(L, target.as_arr());
			return L->ok(target.as_arr()->get(L, index));
		}
		if (target.is_tarr()) {
			msize_t index = 0;
			if (!checked_index(key, index)) [[unlikely]] {
				return L->error("indexing typed array with non-integer or negative key");
			}
			shared::recursive_guard guard(L, target.as_tarr());
			return target.as_tarr()->get(L, index);
		}
		if (target.is_str()) {
			size_t index = 0;
			if (!checked_index(key, index)) [[unlikely]] {
				return L->error("indexing string with non-integer or negative key");
			}
			auto contents = target.as_str()->view();
			return L->ok(contents.size() <= index ? any(nil) : any(number((uint8_t) contents[index])));
		}
		if (target.is_obj()) {
			if (!key.is_str()) [[unlikely]] {
				return L->error("indexing class instance with non-string key");
			}
			object* instance = target.as_obj();
			for (const field_pair& field : instance->cl->fields()) {
				if (!string_value_equals(field.key, key.as_str()))
					continue;
				gc::header*             owner = field.value.is_static ? static_cast<gc::header*>(instance->cl) : static_cast<gc::header*>(instance);
				shared::recursive_guard guard(L, owner);
				return L->ok(instance->get(key.as_str()));
			}
			return L->ok();
		}
		return L->error("indexing non-table");
	}

	any_t LI_CC field_get(vm* L, any_t target, any_t key) {
		if (key == nil) [[unlikely]] {
			return L->error("indexing with null key");
		}

		if (target.is_tbl()) {
			shared::recursive_guard guard(L, target.as_tbl());
			if (target.as_tbl()->contains(key)) {
				return field_get_raw(L, target, key);
			}
		} else if (target.is_arr() || target.is_tarr() || target.is_str()) {
			return field_get_raw(L, target, key);
		} else if (target.is_obj()) {
			if (key.is_str()) {
				object* instance = target.as_obj();
				if (function* getter = pin_class_property(L, instance->cl, key.as_str(), property_access::get))
					return invoke_property(L, getter, target);
				if (class_has_property(instance->cl, key.as_str()))
					return L->error("reading write-only property");
				for (auto& field : instance->cl->fields()) {
					if (string_value_equals(field.key, key.as_str())) {
						return field_get_raw(L, target, key);
					}
				}
			}
		}

		if (has_trait(target, trait::at)) {
			std::array<any, 1> args = {any(key)};
			return invoke_trait(L, trait::at, target, args);
		}
		if (target.is_tbl()) {
			return L->ok();
		}
		if (target.is_obj()) {
			if (!key.is_str()) [[unlikely]] {
				return L->error("indexing class instance with non-string key");
			}
			return L->ok();
		}
		return L->error("indexing non-table");
	}

	any_t LI_CC capture_get(vm* L, function* target, int32_t index) {
		if (index < 0 || msize_t(index) >= target->num_uval)
			L->panic("invalid function capture index");
		if (!target->shared) {
			any result = target->uvals()[index];
			rc::retain_frame(L, result);
			return result;
		}

		shared::recursive_guard guard(L, target);
		any                     result = target->uvals()[index];
		rc::retain_frame(L, result);
		return result;
	}

	any_t LI_CC capture_set(vm* L, function* target, int32_t index, any_t value) {
		if (index < 0 || msize_t(index) >= target->num_uval)
			L->panic("invalid function capture index");
		if (!target->shared) {
			if (!rc::try_replace(L, target->uvals()[index], value))
				return exception_marker;
			return L->ok();
		}

		shared::prepared_value prepared = shared::prepare_store(L, target, value);
		if (!prepared.ok)
			return exception_marker;

		any  previous = nil;
		bool stored   = false;
		{
			shared::recursive_guard guard(L, target);
			if (rc::try_retain(L, prepared.value)) {
				previous               = target->uvals()[index];
				target->uvals()[index] = prepared.value;
				stored                 = true;
			}
		}
		if (stored)
			rc::release(L, previous);
		shared::finish_store(L, prepared);
		if (!stored)
			return exception_marker;
		return L->ok();
	}
};