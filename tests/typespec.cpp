#include <cstdio>
#include <cstdlib>
#include <lang/typespec.hpp>
#include <span>
#include <string>
#include <vector>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>

namespace {
	using namespace li;
	using namespace li::strict;

	[[noreturn]] void fail(const char* message) {
		std::fprintf(stderr, "typespec regression: %s\n", message);
		std::abort();
	}

	void require(bool condition, const char* message) {
		if (!condition)
			fail(message);
	}

	vclass* make_class(vm* L, const char* name, vclass* super = nullptr, bool value_semantics = false) {
		string* class_name = string::create(L, name);
		vclass* result     = vclass::create(L, class_name, std::span<const field_pair>{}, std::span<const uint8_t>{}, std::span<const uint8_t>{}, super,
			 reserve_class_identity(L), value_semantics);
		rc::release(L, class_name);
		return result;
	}

	void test_assignability(vclass* base, vclass* derived, vclass* vec3, vclass* other_struct) {
		const typespec any       = typespec::any();
		const typespec nil       = typespec::nil();
		const typespec boolean   = typespec::boolean();
		const typespec number    = typespec::number();
		const typespec f32       = typespec::float32();
		const typespec f64       = typespec::float64();
		const typespec i8        = typespec::integer(true, 8);
		const typespec i16       = typespec::integer(true, 16);
		const typespec u8        = typespec::integer(false, 8);
		const typespec u16       = typespec::integer(false, 16);
		const typespec base_ref  = typespec::class_type(base);
		const typespec child_ref = typespec::class_type(derived);
		const typespec vec_ref   = typespec::struct_type(vec3);

		require(assignable(any, nil) && assignable(any, vec_ref), "any did not accept every source type");
		require(!assignable(boolean, any), "dynamic any implicitly narrowed to bool");
		require(same(number, f64) && assignable(number, f64) && assignable(f64, number), "number and f64 were not identical");
		require(assignable(number, f32) && assignable(f64, f32) && !assignable(f32, f64), "f32/f64 widening was incorrect");

		require(assignable(i16, i8) && !assignable(i8, i16), "signed integer widening was incorrect");
		require(assignable(u16, u8) && !assignable(u8, u16), "unsigned integer widening was incorrect");
		require(assignable(i16, u8) && !assignable(i8, u8), "unsigned-to-signed representability was incorrect");
		require(!assignable(u16, i8), "signed integer widened to an unsigned type");

		const typespec maybe_number = typespec::optional_of(number);
		require(assignable(maybe_number, nil) && assignable(maybe_number, f64), "optional rejected nil or its value type");
		require(!assignable(number, maybe_number) && !assignable(number, nil), "optional or nil narrowed to a non-optional type");

		const typespec view_base  = typespec::view_of(base_ref);
		const typespec view_child = typespec::view_of(child_ref);
		require(assignable(view_base, base_ref) && assignable(view_base, child_ref), "view did not borrow a compatible owned value");
		require(assignable(view_base, view_child), "view did not accept a compatible borrowed value");
		require(!assignable(base_ref, view_base), "borrowed view escaped into owned storage");

		require(assignable(base_ref, child_ref), "class reference rejected a subclass");
		require(!assignable(child_ref, base_ref), "class reference accepted a superclass as a subclass");
		require(assignable(vec_ref, typespec::struct_type(vec3)), "struct reference rejected its exact class");
		require(!assignable(vec_ref, typespec::struct_type(other_struct)), "struct reference accepted a different class");

		const typespec generic = typespec::generic(0);
		require(!assignable(generic, generic) && !assignable(generic, i8), "generic parameter bypassed unification");
	}

	void test_binding(vclass* pair_class) {
		const typespec        parameter = typespec::generic(0);
		const typespec        pattern   = typespec::struct_type(pair_class, {parameter, parameter});
		const typespec        i32       = typespec::integer(true, 32);
		const typespec        good      = typespec::struct_type(pair_class, {i32, i32});
		const typespec        conflict  = typespec::struct_type(pair_class, {i32, typespec::string_type()});
		std::vector<typespec> bindings;

		require(bind(pattern, good, bindings), "generic unification rejected consistent bindings");
		require(bindings.size() == 1 && same(bindings[0], i32), "generic unification recorded the wrong binding");

		bindings.clear();
		require(!bind(pattern, conflict, bindings), "generic unification accepted conflicting bindings");
		require(bindings.empty(), "failed generic unification modified the caller's bindings");

		const typespec nested_pattern = typespec::typed_array_of(typespec::optional_of(parameter));
		const typespec nested_actual  = typespec::typed_array_of(typespec::optional_of(typespec::float32()));
		require(bind(nested_pattern, nested_actual, bindings), "nested generic unification failed");
		require(bindings.size() == 1 && same(bindings[0], typespec::float32()), "nested unification bound the wrong type");
	}

	void test_boxability() {
		std::string reason;
		require(boxable_to_dynamic(typespec::integer(true, 54), &reason), "exact signed integer range was rejected");
		require(boxable_to_dynamic(typespec::integer(false, 53), &reason), "exact unsigned integer range was rejected");
		require(!boxable_to_dynamic(typespec::integer(true, 55), &reason) && reason.find("2^53") != std::string::npos,
			 "out-of-range signed integer was boxable without a useful reason");
		require(!boxable_to_dynamic(typespec::integer(false, 54), &reason) && reason.find("2^53") != std::string::npos,
			 "out-of-range unsigned integer was boxable without a useful reason");
		require(!boxable_to_dynamic(typespec::integer(false, 64), &reason) && reason.find("2^63") != std::string::npos, "u64 boundary hazard was not reported");
	}

	void test_printing(vm* L, vclass* vec3) {
		string* generic_name = string::create(L, "T");
		require(to_string(typespec::integer(true, 32)) == "i32", "signed integer spelling was incorrect");
		require(to_string(typespec::float32()) == "f32", "f32 spelling was incorrect");
		require(to_string(typespec::optional_of(typespec::generic(0, generic_name))) == "T?", "optional spelling was incorrect");
		require(to_string(typespec::view_of(typespec::generic(0, generic_name))) == "view T", "view spelling was incorrect");
		require(to_string(typespec::weak(typespec::generic(0, generic_name))) == "weak T", "weak spelling was incorrect");
		require(to_string(typespec::typed_array_of(typespec::generic(0, generic_name))) == "T[]", "typed-array spelling was incorrect");
		require(to_string(typespec::fixed_array_of(typespec::generic(0, generic_name), 4)) == "T[4]", "fixed-array spelling was incorrect");
		require(to_string(typespec::struct_type(vec3, {typespec::float32()})) == "Vec3<f32>", "instantiated struct spelling was incorrect");
		rc::release(L, generic_name);
	}
}

int main() {
	using namespace li;

	vm* L = vm::create();
	require(L != nullptr, "failed to create VM");
	vclass* base         = make_class(L, "Base");
	vclass* derived      = make_class(L, "Derived", base);
	vclass* vec3         = make_class(L, "Vec3", nullptr, true);
	vclass* other_struct = make_class(L, "Other", nullptr, true);
	vclass* pair_class   = make_class(L, "Pair", nullptr, true);

	test_assignability(base, derived, vec3, other_struct);
	test_binding(pair_class);
	test_boxability();
	test_printing(L, vec3);

	require(dynamic_storage(typespec::integer(false, 32)) == type::i64, "unsigned storage did not use i64");
	require(dynamic_storage(typespec::float32()) == type::f32, "f32 storage mapping was incorrect");
	require(dynamic_storage(typespec::struct_type(vec3)) == type::obj, "struct storage mapping was incorrect");
	require(dynamic_guard(typespec::struct_type(vec3)) == type_invalid, "identity guard did not request a class check");
	require(dynamic_guard(typespec::typed_array_of(typespec::float32())) == type_typed_array, "typed-array guard was incorrect");
	require(specificity(typespec::integer(true, 32)) > specificity(typespec::generic(0)), "concrete type was not more specific than a generic");
	require(specificity(typespec::class_type(derived)) > specificity(typespec::class_type(base)), "exact subclass did not outrank its base");

	rc::release(L, pair_class);
	rc::release(L, other_struct);
	rc::release(L, vec3);
	rc::release(L, derived);
	rc::release(L, base);
	L->close();
	return 0;
}
