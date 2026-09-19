#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <vm/types.hpp>

namespace li::strict {
	enum class type_kind : uint8_t {
		any,
		nil,
		boolean,
		number,
		string,
		table,
		array,
		function,
		weak_ref,
		class_ref,
		struct_ref,
		typed_array,
		fixed_array,
		optional,
		view,
		generic_param,
		sized_int,
		f32,
		f64,
		union_type,
		self_ref,
	};

	struct typespec {
		type_kind             kind          = type_kind::any;
		bool                  is_signed     = false;
		uint8_t               bits          = 0;
		msize_t               count         = 0;
		vclass*               declared      = nullptr;
		uint32_t              generic_index = 0;
		string*               generic_name  = nullptr;
		std::vector<typespec> children;

		typespec() = default;
		explicit typespec(type_kind kind) : kind(kind) {}
		typespec(type_kind kind, std::vector<typespec> children) : kind(kind), children(std::move(children)) {}

		static typespec any() { return typespec(type_kind::any); }
		static typespec nil() { return typespec(type_kind::nil); }
		static typespec boolean() { return typespec(type_kind::boolean); }
		static typespec number() { return typespec(type_kind::number); }
		static typespec string_type() { return typespec(type_kind::string); }
		static typespec table() { return typespec(type_kind::table); }
		static typespec array() { return typespec(type_kind::array); }
		static typespec function(std::vector<typespec> signature = {}) { return typespec(type_kind::function, std::move(signature)); }
		static typespec weak(typespec value) { return typespec(type_kind::weak_ref, {std::move(value)}); }
		static typespec class_type(vclass* declared, std::vector<typespec> arguments = {});
		static typespec struct_type(vclass* declared, std::vector<typespec> arguments = {});
		static typespec typed_array_of(typespec element) { return typespec(type_kind::typed_array, {std::move(element)}); }
		static typespec fixed_array_of(typespec element, msize_t count);
		static typespec optional_of(typespec value) { return typespec(type_kind::optional, {std::move(value)}); }
		static typespec view_of(typespec value) { return typespec(type_kind::view, {std::move(value)}); }
		static typespec generic(uint32_t index, string* name = nullptr);
		static typespec integer(bool is_signed, uint8_t bits);
		static typespec float32() { return typespec(type_kind::f32); }
		static typespec float64() { return typespec(type_kind::f64); }
		static typespec union_of(std::vector<typespec> alternatives) { return typespec(type_kind::union_type, std::move(alternatives)); }
		static typespec self(vclass* declared = nullptr);
	};

	bool        same(const typespec& left, const typespec& right);
	bool        assignable(const typespec& to, const typespec& from);
	type        dynamic_storage(const typespec& value);
	value_type  dynamic_guard(const typespec& value);
	bool        boxable_to_dynamic(const typespec& value, std::string* reason = nullptr);
	std::string to_string(const typespec& value);
	typespec    parse_from_dynamic(value_type value);
	bool        bind(const typespec& pattern, const typespec& actual, std::vector<typespec>& bindings);
	int         specificity(const typespec& value);
}
