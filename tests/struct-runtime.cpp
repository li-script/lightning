#include <array>
#include <cstdio>
#include <cstdlib>
#include <lib/std.hpp>
#include <string_view>
#include <vm/array.hpp>
#include <vm/iterator.hpp>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/shared.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <vm/typed_array.hpp>

namespace {
	using namespace li;

	[[noreturn]] void fail(const char* message) {
		std::fprintf(stderr, "struct runtime regression: %s\n", message);
		std::abort();
	}

	void require(bool condition, const char* message) {
		if (!condition)
			fail(message);
	}

	string* text(vm* L, std::string_view value) { return string::create(L, value); }

	struct leaf_fixture {
		vclass* cl         = nullptr;
		string* number_key = nullptr;
		string* label_key  = nullptr;
	};

	leaf_fixture create_leaf_class(vm* L) {
		leaf_fixture result;
		result.number_key   = text(L, "number");
		result.label_key    = text(L, "label");
		string*    name     = text(L, "NativeLeaf");
		field_pair fields[] = {
			 {result.number_key, field_info{.ty = type::f64, .offset = 0}},
			 {result.label_key, field_info{.ty = type::str, .offset = 8}},
		};
		std::array<uint8_t, 16> defaults{};
		result.cl              = vclass::create(L, name, fields, defaults, {}, nullptr, reserve_class_identity(L), true);
		string* attribute_name = text(L, "NativeAttr");
		string* attribute_text = text(L, "metadata");
		array*  attribute_args = array::create(L, 2);
		require(attribute_args->set(L, 0, any(attribute_text)), "failed to set string attribute argument");
		require(attribute_args->set(L, 1, any(number(5))), "failed to set numeric attribute argument");
		table* attributes = table::create(L);
		require(attributes->set(L, any(attribute_name), any(attribute_args)), "failed to set class attribute metadata");
		attributes->is_frozen = true;
		result.cl->attributes = attributes;
		rc::retain(attributes);
		rc::release(L, attributes);
		rc::release(L, attribute_args);
		rc::release(L, attribute_text);
		rc::release(L, attribute_name);
		rc::release(L, name);
		return result;
	}

	object* make_leaf(vm* L, const leaf_fixture& fixture, number value, std::string_view label) {
		object* result      = object::create(L, fixture.cl);
		string* value_label = text(L, label);
		require(result->set(L, fixture.number_key, any(value)), "failed to set leaf number");
		require(result->set(L, fixture.label_key, any(value_label)), "failed to set leaf label");
		rc::release(L, value_label);
		return result;
	}

	void copy_and_equality(vm* L, const leaf_fixture& leaf) {
		object* original      = make_leaf(L, leaf, 7, "same-content");
		original->finalizable = false;
		object* copy          = original->copy(L);
		require(copy != original, "object::copy preserved identity");
		require(copy->finalizable, "object::copy did not complete construction lifecycle");
		require(object_equals(L, original, copy), "copied struct was not fieldwise equal");

		string* changed = text(L, "changed");
		require(copy->set(L, leaf.number_key, any(number(9))), "failed to mutate copied number");
		require(copy->set(L, leaf.label_key, any(changed)), "failed to mutate copied label");
		rc::release(L, changed);
		require(original->get(leaf.number_key).as_num() == 7, "copy mutation changed original numeric field");
		require(original->get(leaf.label_key).as_str()->view() == "same-content", "copy mutation changed original reference field");
		require(!object_equals(L, original, copy), "different structs compared equal");

		object* same = make_leaf(L, leaf, 7, "same-content");
		require(object_equals(L, original, same), "strings did not compare by content");

		string*    holder_name     = text(L, "NativeHolder");
		string*    nested_key      = text(L, "nested");
		string*    identity_key    = text(L, "identity");
		field_pair holder_fields[] = {
			 {nested_key, field_info{.ty = type::any, .offset = 0}},
			 {identity_key, field_info{.ty = type::any, .offset = 8}},
		};
		std::array<uint8_t, 16> holder_defaults{};
		any(nil).store_at(holder_defaults.data(), type::any);
		any(nil).store_at(holder_defaults.data() + 8, type::any);
		vclass* holder_class = vclass::create(L, holder_name, holder_fields, holder_defaults, {}, nullptr, reserve_class_identity(L), true);
		rc::release(L, holder_name);

		object* left_holder  = object::create(L, holder_class);
		object* right_holder = object::create(L, holder_class);
		object* left_nested  = make_leaf(L, leaf, 3, "nested");
		object* right_nested = make_leaf(L, leaf, 3, "nested");
		table*  identity     = table::create(L);
		require(left_holder->set(L, nested_key, any(left_nested)), "failed to set left nested struct");
		require(right_holder->set(L, nested_key, any(right_nested)), "failed to set right nested struct");
		require(left_holder->set(L, identity_key, any(identity)), "failed to set left identity field");
		require(right_holder->set(L, identity_key, any(identity)), "failed to set right identity field");
		require(object_equals(L, left_holder, right_holder), "nested structs did not compare recursively");

		table* other_identity = table::create(L);
		require(right_holder->set(L, identity_key, any(other_identity)), "failed to replace identity field");
		require(!object_equals(L, left_holder, right_holder), "ordinary GC fields did not compare by identity");

		rc::release(L, other_identity);
		rc::release(L, identity);
		rc::release(L, right_nested);
		rc::release(L, left_nested);
		rc::release(L, right_holder);
		rc::release(L, left_holder);
		rc::release(L, holder_class);
		rc::release(L, identity_key);
		rc::release(L, nested_key);
		rc::release(L, same);
		rc::release(L, copy);
		rc::release(L, original);
	}

	void typed_struct_arrays(vm* L, const leaf_fixture& leaf) {
		object*      first  = make_leaf(L, leaf, 11, "eleven");
		object*      second = make_leaf(L, leaf, 22, "twenty-two");
		typed_array* values = typed_array::create(L, leaf.cl, 2);
		require(!values->set(L, 0, any(first)).is_exc(), "failed to set first struct array element");
		require(!values->set(L, 1, any(second)).is_exc(), "failed to set second struct array element");
		require(values->length == 2 && values->element_size() == 16, "struct array layout was incorrect");

		any fetched_value = values->get(L, 0);
		require(fetched_value.is_obj(), "struct array get did not materialize an object");
		object* fetched = fetched_value.as_obj();
		require(object_equals(L, fetched, first), "materialized struct did not match stored fields");
		require(fetched->set(L, leaf.number_key, any(number(99))), "failed to mutate materialized struct");
		any fetched_again_value = values->get(L, 0);
		require(fetched_again_value.is_obj() && fetched_again_value.as_obj()->get(leaf.number_key).as_num() == 11,
			 "materialized struct aliased inline array storage");

		typed_array* duplicate = values->duplicate(L);
		require(duplicate != values && duplicate->length == values->length, "struct array dup did not preserve shape");
		require(!duplicate->set(L, 0, any(second)).is_exc(), "failed to mutate duplicate struct array");
		any original_zero = values->get(L, 0);
		require(original_zero.is_obj() && original_zero.as_obj()->get(leaf.number_key).as_num() == 11, "struct array dup aliased source storage");

		any     state = nil;
		any     key   = nil;
		any     item  = nil;
		number  sum   = 0;
		msize_t count = 0;
		while (true) {
			any advanced = iterator_step(L, any(values), state, key, item);
			require(!advanced.is_exc(), "struct array iteration failed");
			if (!advanced.as_bool())
				break;
			require(item.is_obj(), "struct array iterator did not materialize a struct");
			sum += item.as_obj()->get(leaf.number_key).as_num();
			++count;
		}
		require(count == 2 && sum == 33, "struct array iterator returned incorrect elements");
		rc::clear(L, item);
		rc::clear(L, key);
		rc::clear(L, state);

		any shared_class_value = shared::make(L, any(leaf.cl));
		require(shared_class_value.is_vcl() && shared::is_shared(shared_class_value), "struct class did not clone into shared heap");
		require(shared_class_value.as_vcl()->value_semantics, "shared struct class lost value semantics");
		require(shared_class_value.as_vcl()->attributes && shared_class_value.as_vcl()->attributes->is_frozen, "shared struct class lost immutable attributes");
		string* attribute_name        = text(L, "NativeAttr");
		any     shared_attribute_args = shared_class_value.as_vcl()->attributes->get(L, any(attribute_name));
		require(shared_attribute_args.is_arr() && shared::is_shared(shared_attribute_args), "shared class did not deep-clone attribute arguments");
		rc::release(L, attribute_name);
		any shared_array_value = shared::make(L, any(values));
		require(shared_array_value.is_tarr() && shared::is_shared(shared_array_value), "struct array did not clone into shared heap");
		typed_array* shared_values = shared_array_value.as_tarr();
		require(shared_values->is_struct_array() && shared_values->element_class && shared_values->element_class->value_semantics,
			 "shared struct array lost element metadata");
		any shared_element = shared_values->get(L, 1);
		require(shared_element.is_obj() && shared_element.as_obj()->get(leaf.number_key).as_num() == 22, "shared struct array lost inline element data");
		require(object_equals(L, shared_element.as_obj(), second), "shared class clone lost nominal struct equality");
		require(!shared_values->set(L, 0, any(second)).is_exc(), "shared struct array rejected a shareable value copy");
		shared_values->resize(L, 3);
		any shared_default = shared_values->get(L, 2);
		require(shared_default.is_obj() && shared_default.as_obj()->get(leaf.number_key).as_num() == 0, "shared struct array resize lost class defaults");

		rc::release(L, shared_default);
		rc::release(L, shared_element);
		rc::release(L, shared_array_value);
		rc::release(L, shared_class_value);
		rc::release(L, original_zero);
		rc::release(L, duplicate);
		rc::release(L, fetched_again_value);
		rc::release(L, fetched_value);
		rc::release(L, values);
		rc::release(L, second);
		rc::release(L, first);
	}
}

int main() {
	li::vm* L = li::vm::create();
	require(L != nullptr, "failed to create VM");
	li::lib::register_std(L);
	const uint64_t baseline = L->gc.live_objects;

	leaf_fixture leaf = create_leaf_class(L);
	copy_and_equality(L, leaf);
	typed_struct_arrays(L, leaf);

	li::rc::release(L, leaf.cl);
	li::rc::release(L, leaf.label_key);
	li::rc::release(L, leaf.number_key);
	require(L->gc.live_objects == baseline, "struct runtime leaked private VM objects");
	L->close();
	return 0;
}
