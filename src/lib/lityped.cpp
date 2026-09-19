#include <lib/std.hpp>
#include <util/user.hpp>
#include <vm/array.hpp>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/shared.hpp>
#include <vm/string.hpp>
#include <vm/typed_array.hpp>

namespace li::lib {
	namespace {
		any_t construct_typed(vm* L, any* args, slot_t count, typed_array_kind kind) {
			if (count == 0)
				return L->take(typed_array::create(L, kind));
			if (count != 1)
				return L->error("%s[] constructor expects zero or one argument", typed_array_kind_name(kind));

			if (args[0].is_num()) {
				typed_array* result  = typed_array::create(L, kind);
				any_t        resized = result->resize(L, args[0]);
				if (resized.is_exc()) {
					rc::release(L, result);
					return resized;
				}
				return L->take(result);
			}

			if (args[0].is_arr()) {
				array*       source = args[0].as_arr();
				typed_array* result = typed_array::create(L, kind, source->length);
				for (msize_t index = 0; index != source->length; ++index) {
					any_t stored = result->set(L, index, source->begin()[index]);
					if (stored.is_exc()) {
						rc::release(L, result);
						return stored;
					}
				}
				return L->take(result);
			}

			return L->error("%s[] constructor expects a length or array", typed_array_kind_name(kind));
		}

		any_t construct_struct_array(vm* L, any* args, slot_t count) {
			if (count != 2 || !args[0].is_vcl())
				return L->error("typed.struct_array expects a struct class and length");
			vclass* cl = args[0].as_vcl();
			if (!cl->value_semantics)
				return L->error("typed.struct_array requires a struct class");
			typed_array* result  = typed_array::create(L, cl);
			any_t        resized = result->resize(L, args[-1]);
			if (resized.is_exc()) {
				rc::release(L, result);
				return resized;
			}
			return L->take(result);
		}

		typed_array* expect_typed(vm* L, any* args, slot_t count, slot_t expected, const char* operation) {
			if (count != expected || !args[0].is_tarr()) {
				L->error("typed.%s expects a typed array%s", operation, expected == 1 ? "" : " and arguments");
				return nullptr;
			}
			return args[0].as_tarr();
		}

		any_t typed_get(vm* L, any* args, slot_t count) {
			typed_array* value = expect_typed(L, args, count, 2, "get");
			if (!value)
				return exception_marker;
			return value->get(L, args[-1]);
		}

		any_t typed_set(vm* L, any* args, slot_t count) {
			typed_array* value = expect_typed(L, args, count, 3, "set");
			if (!value)
				return exception_marker;
			return value->set(L, args[-1], args[-2]);
		}

		any_t typed_len(vm* L, any* args, slot_t count) {
			typed_array* value = expect_typed(L, args, count, 1, "len");
			if (!value)
				return exception_marker;
			shared::recursive_guard guard(L, value);
			return L->ok(number(value->length));
		}

		any_t typed_capacity(vm* L, any* args, slot_t count) {
			typed_array* value = expect_typed(L, args, count, 1, "capacity");
			if (!value)
				return exception_marker;
			shared::recursive_guard guard(L, value);
			return L->ok(number(value->capacity));
		}

		any_t typed_element_size(vm* L, any* args, slot_t count) {
			typed_array* value = expect_typed(L, args, count, 1, "element_size");
			if (!value)
				return exception_marker;
			shared::recursive_guard guard(L, value);
			return L->ok(number(value->element_size()));
		}

		any_t typed_kind(vm* L, any* args, slot_t count) {
			typed_array* value = expect_typed(L, args, count, 1, "kind");
			if (!value)
				return exception_marker;
			return L->take(string::create(L, typed_array_kind_name(value->element_kind)));
		}

		any_t typed_reserve(vm* L, any* args, slot_t count) {
			typed_array* value = expect_typed(L, args, count, 2, "reserve");
			if (!value)
				return exception_marker;
			return value->reserve(L, args[-1]);
		}

		any_t typed_resize(vm* L, any* args, slot_t count) {
			typed_array* value = expect_typed(L, args, count, 2, "resize");
			if (!value)
				return exception_marker;
			return value->resize(L, args[-1]);
		}

		any_t typed_fill(vm* L, any* args, slot_t count) {
			typed_array* value = expect_typed(L, args, count, 2, "fill");
			if (!value)
				return exception_marker;
			return value->fill(L, args[-1]);
		}

		any_t typed_dup(vm* L, any* args, slot_t count) {
			typed_array* value = expect_typed(L, args, count, 1, "dup");
			if (!value)
				return exception_marker;
			return L->take(value->duplicate(L));
		}
	}

	void register_typed(vm* L) {
#define LI_REGISTER_TYPED_CONSTRUCTOR(Name) \
	util::export_as(L, "typed." #Name, [](vm* vm, any* args, slot_t count) { return construct_typed(vm, args, count, typed_array_kind::Name); })
		LI_REGISTER_TYPED_CONSTRUCTOR(i8);
		LI_REGISTER_TYPED_CONSTRUCTOR(u8);
		LI_REGISTER_TYPED_CONSTRUCTOR(i16);
		LI_REGISTER_TYPED_CONSTRUCTOR(u16);
		LI_REGISTER_TYPED_CONSTRUCTOR(i32);
		LI_REGISTER_TYPED_CONSTRUCTOR(u32);
		LI_REGISTER_TYPED_CONSTRUCTOR(i64);
		LI_REGISTER_TYPED_CONSTRUCTOR(u64);
		LI_REGISTER_TYPED_CONSTRUCTOR(f32);
		LI_REGISTER_TYPED_CONSTRUCTOR(f64);
#undef LI_REGISTER_TYPED_CONSTRUCTOR

		util::export_as(L, "typed.get", typed_get);
		util::export_as(L, "typed.struct_array", construct_struct_array);
		util::export_as(L, "typed.set", typed_set);
		util::export_as(L, "typed.len", typed_len);
		util::export_as(L, "typed.capacity", typed_capacity);
		util::export_as(L, "typed.element_size", typed_element_size);
		util::export_as(L, "typed.kind", typed_kind);
		util::export_as(L, "typed.reserve", typed_reserve);
		util::export_as(L, "typed.resize", typed_resize);
		util::export_as(L, "typed.fill", typed_fill);
		util::export_as(L, "typed.dup", typed_dup);
	}
}
