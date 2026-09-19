#include <algorithm>
#include <lang/typespec.hpp>
#include <limits>
#include <vm/object.hpp>
#include <vm/string.hpp>

namespace li::strict {
	namespace {
		constexpr uint32_t unbound_generic = std::numeric_limits<uint32_t>::max();

		bool is_f64_kind(type_kind kind) { return kind == type_kind::number || kind == type_kind::f64; }

		bool same_declared(const vclass* left, const vclass* right) {
			return left == right || (left && right && left->identity && left->identity == right->identity);
		}

		bool is_subclass(const vclass* derived, const vclass* base) {
			if (!base)
				return true;
			for (const vclass* current = derived; current; current = current->super) {
				if (same_declared(current, base))
					return true;
			}
			return false;
		}

		bool same_children(const typespec& left, const typespec& right) {
			if (left.children.size() != right.children.size())
				return false;
			for (size_t index = 0; index != left.children.size(); ++index) {
				if (!same(left.children[index], right.children[index]))
					return false;
			}
			return true;
		}

		bool assignable_children(const typespec& to, const typespec& from) {
			if (to.children.size() != from.children.size())
				return false;
			for (size_t index = 0; index != to.children.size(); ++index) {
				if (!assignable(to.children[index], from.children[index]))
					return false;
			}
			return true;
		}

		std::string declared_name(const typespec& value, std::string_view fallback) {
			if (value.declared && value.declared->name)
				return std::string(value.declared->name->view());
			return std::string(fallback);
		}

		std::string named_type(const typespec& value, std::string_view fallback) {
			std::string result = declared_name(value, fallback);
			if (!value.children.empty()) {
				result += '<';
				for (size_t index = 0; index != value.children.size(); ++index) {
					if (index)
						result += ", ";
					result += to_string(value.children[index]);
				}
				result += '>';
			}
			return result;
		}

		bool bind_impl(const typespec& pattern, const typespec& actual, std::vector<typespec>& bindings) {
			if (pattern.kind == type_kind::generic_param) {
				if (pattern.generic_index == unbound_generic)
					return false;
				const size_t index = pattern.generic_index;
				if (index >= bindings.size())
					bindings.resize(index + 1, typespec::generic(unbound_generic));
				if (bindings[index].kind == type_kind::generic_param && bindings[index].generic_index == unbound_generic) {
					bindings[index] = actual;
					return true;
				}
				return same(bindings[index], actual);
			}

			if (pattern.kind == type_kind::union_type) {
				for (const typespec& alternative : pattern.children) {
					auto trial = bindings;
					if (bind_impl(alternative, actual, trial)) {
						bindings = std::move(trial);
						return true;
					}
				}
				return false;
			}

			if (pattern.kind == type_kind::optional) {
				if (actual.kind == type_kind::nil)
					return true;
				if (pattern.children.size() != 1)
					return false;
				if (actual.kind == type_kind::optional)
					return actual.children.size() == 1 && bind_impl(pattern.children[0], actual.children[0], bindings);
				return bind_impl(pattern.children[0], actual, bindings);
			}

			if (pattern.kind == type_kind::view) {
				if (pattern.children.size() != 1)
					return false;
				if (actual.kind == type_kind::view)
					return actual.children.size() == 1 && bind_impl(pattern.children[0], actual.children[0], bindings);
				return bind_impl(pattern.children[0], actual, bindings);
			}

			if (pattern.children.empty())
				return assignable(pattern, actual);
			if (pattern.children.size() != actual.children.size())
				return false;
			typespec pattern_outer = pattern;
			typespec actual_outer  = actual;
			pattern_outer.children.clear();
			actual_outer.children.clear();
			if (!assignable(pattern_outer, actual_outer))
				return false;
			for (size_t index = 0; index != pattern.children.size(); ++index) {
				if (!bind_impl(pattern.children[index], actual.children[index], bindings))
					return false;
			}
			return true;
		}
	}

	typespec typespec::class_type(vclass* declared, std::vector<typespec> arguments) {
		typespec result(type_kind::class_ref, std::move(arguments));
		result.declared = declared;
		return result;
	}

	typespec typespec::struct_type(vclass* declared, std::vector<typespec> arguments) {
		typespec result(type_kind::struct_ref, std::move(arguments));
		result.declared = declared;
		return result;
	}

	typespec typespec::fixed_array_of(typespec element, msize_t count) {
		typespec result(type_kind::fixed_array, {std::move(element)});
		result.count = count;
		return result;
	}

	typespec typespec::generic(uint32_t index, string* name) {
		typespec result(type_kind::generic_param);
		result.generic_index = index;
		result.generic_name  = name;
		return result;
	}

	typespec typespec::integer(bool is_signed, uint8_t bits) {
		typespec result(type_kind::sized_int);
		result.is_signed = is_signed;
		result.bits      = bits;
		return result;
	}

	typespec typespec::self(vclass* declared) {
		typespec result(type_kind::self_ref);
		result.declared = declared;
		return result;
	}

	bool same(const typespec& left, const typespec& right) {
		if (is_f64_kind(left.kind) && is_f64_kind(right.kind))
			return true;
		if (left.kind != right.kind)
			return false;

		switch (left.kind) {
			case type_kind::sized_int:
				return left.is_signed == right.is_signed && left.bits == right.bits;
			case type_kind::fixed_array:
				return left.count == right.count && same_children(left, right);
			case type_kind::class_ref:
			case type_kind::struct_ref:
			case type_kind::self_ref:
				return same_declared(left.declared, right.declared) && same_children(left, right);
			case type_kind::generic_param:
				return left.generic_index == right.generic_index;
			default:
				return same_children(left, right);
		}
	}

	bool assignable(const typespec& to, const typespec& from) {
		if (to.kind == type_kind::generic_param || from.kind == type_kind::generic_param)
			return false;
		if (same(to, from) || to.kind == type_kind::any)
			return true;

		if (to.kind == type_kind::union_type) {
			return std::any_of(to.children.begin(), to.children.end(), [&](const typespec& alternative) { return assignable(alternative, from); });
		}
		if (from.kind == type_kind::union_type) {
			return !from.children.empty() &&
					 std::all_of(from.children.begin(), from.children.end(), [&](const typespec& alternative) { return assignable(to, alternative); });
		}

		if (to.kind == type_kind::optional) {
			if (from.kind == type_kind::nil)
				return true;
			if (to.children.size() != 1)
				return false;
			if (from.kind == type_kind::optional)
				return from.children.size() == 1 && assignable(to.children[0], from.children[0]);
			return assignable(to.children[0], from);
		}
		if (from.kind == type_kind::optional)
			return false;

		if (to.kind == type_kind::view) {
			if (to.children.size() != 1)
				return false;
			if (from.kind == type_kind::view)
				return from.children.size() == 1 && assignable(to.children[0], from.children[0]);
			return assignable(to.children[0], from);
		}
		if (from.kind == type_kind::view)
			return false;

		if (to.kind == type_kind::sized_int && from.kind == type_kind::sized_int) {
			if (!to.bits || !from.bits || to.bits > 64 || from.bits > 64)
				return false;
			if (to.is_signed == from.is_signed)
				return to.bits >= from.bits;
			if (to.is_signed && !from.is_signed)
				return to.bits > from.bits;
			return false;
		}
		if (is_f64_kind(to.kind) && from.kind == type_kind::f32)
			return true;

		if (to.kind == type_kind::class_ref && from.kind == type_kind::class_ref) {
			if (!is_subclass(from.declared, to.declared))
				return false;
			return to.children.empty() || assignable_children(to, from);
		}
		if (to.kind == type_kind::struct_ref && from.kind == type_kind::struct_ref)
			return same_declared(to.declared, from.declared) && same_children(to, from);
		if (to.kind == type_kind::self_ref && from.kind == type_kind::self_ref)
			return same_declared(to.declared, from.declared);

		if (to.kind != from.kind)
			return false;
		switch (to.kind) {
			case type_kind::weak_ref:
				return assignable_children(to, from);
			case type_kind::typed_array:
				return same_children(to, from);
			case type_kind::fixed_array:
				return to.count == from.count && same_children(to, from);
			case type_kind::function:
				return same_children(to, from);
			default:
				return false;
		}
	}

	type dynamic_storage(const typespec& value) {
		switch (value.kind) {
			case type_kind::sized_int:
				if (!value.is_signed)
					return type::i64;
				if (value.bits <= 8)
					return type::i8;
				if (value.bits <= 16)
					return type::i16;
				if (value.bits <= 32)
					return type::i32;
				return type::i64;
			case type_kind::f32:
				return type::f32;
			case type_kind::number:
			case type_kind::f64:
				return type::f64;
			case type_kind::class_ref:
			case type_kind::struct_ref:
			case type_kind::self_ref:
				return type::obj;
			default:
				return type::any;
		}
	}

	value_type dynamic_guard(const typespec& value) {
		switch (value.kind) {
			case type_kind::nil:
				return type_nil;
			case type_kind::boolean:
				return type_bool;
			case type_kind::number:
			case type_kind::sized_int:
			case type_kind::f32:
			case type_kind::f64:
				return type_number;
			case type_kind::string:
				return type_string;
			case type_kind::table:
				return type_table;
			case type_kind::array:
				return type_array;
			case type_kind::function:
				return type_function;
			case type_kind::weak_ref:
				return type_weak;
			case type_kind::typed_array:
			case type_kind::fixed_array:
				return type_typed_array;
			case type_kind::view:
				return value.children.size() == 1 ? dynamic_guard(value.children[0]) : type_invalid;
			case type_kind::class_ref:
			case type_kind::struct_ref:
			case type_kind::self_ref:
				return value.declared ? type_invalid : type_object;
			default:
				return type_invalid;
		}
	}

	bool boxable_to_dynamic(const typespec& value, std::string* reason) {
		auto reject = [&](const char* message) {
			if (reason)
				*reason = message;
			return false;
		};
		if (reason)
			reason->clear();

		switch (value.kind) {
			case type_kind::sized_int:
				if (!value.bits || value.bits > 64)
					return reject("integer width is not representable by the runtime");
				if (value.is_signed && value.bits > 54)
					return reject("signed integer range exceeds the exact dynamic-number range (±2^53)");
				if (!value.is_signed && value.bits > 53)
					return reject(value.bits == 64 ? "u64 values at or above 2^63 cannot be boxed, and its range exceeds 2^53"
															 : "unsigned integer range exceeds the exact dynamic-number range (2^53)");
				return true;
			case type_kind::generic_param:
				return reject("unbound generic type cannot be boxed");
			case type_kind::optional:
				return value.children.size() == 1 ? boxable_to_dynamic(value.children[0], reason) : reject("optional has no value type");
			case type_kind::view:
				return value.children.size() == 1 ? boxable_to_dynamic(value.children[0], reason) : reject("view has no referenced type");
			case type_kind::union_type:
				for (const typespec& alternative : value.children) {
					if (!boxable_to_dynamic(alternative, reason))
						return false;
				}
				return true;
			default:
				return true;
		}
	}

	std::string to_string(const typespec& value) {
		switch (value.kind) {
			case type_kind::any:
				return "any";
			case type_kind::nil:
				return "nil";
			case type_kind::boolean:
				return "bool";
			case type_kind::number:
				return "number";
			case type_kind::string:
				return "string";
			case type_kind::table:
				return "table";
			case type_kind::array:
				return "array";
			case type_kind::function:
				return "function";
			case type_kind::weak_ref:
				return value.children.size() == 1 ? "weak " + to_string(value.children[0]) : "weak any";
			case type_kind::class_ref:
				return named_type(value, "object");
			case type_kind::struct_ref:
				return named_type(value, "struct");
			case type_kind::typed_array:
				return value.children.size() == 1 ? to_string(value.children[0]) + "[]" : "any[]";
			case type_kind::fixed_array:
				return (value.children.size() == 1 ? to_string(value.children[0]) : "any") + '[' + std::to_string(value.count) + ']';
			case type_kind::optional:
				return value.children.size() == 1 ? to_string(value.children[0]) + '?' : "any?";
			case type_kind::view:
				return value.children.size() == 1 ? "view " + to_string(value.children[0]) : "view any";
			case type_kind::generic_param:
				if (value.generic_name)
					return std::string(value.generic_name->view());
				return "T" + std::to_string(value.generic_index);
			case type_kind::sized_int:
				return std::string(value.is_signed ? "i" : "u") + std::to_string(value.bits);
			case type_kind::f32:
				return "f32";
			case type_kind::f64:
				return "f64";
			case type_kind::union_type: {
				std::string result;
				for (size_t index = 0; index != value.children.size(); ++index) {
					if (index)
						result += " | ";
					result += to_string(value.children[index]);
				}
				return result.empty() ? "never" : result;
			}
			case type_kind::self_ref:
				return "Self";
		}
		return "any";
	}

	typespec parse_from_dynamic(value_type value) {
		switch (value) {
			case type_object:
				return typespec::class_type(nullptr);
			case type_table:
				return typespec::table();
			case type_array:
				return typespec::array();
			case type_function:
				return typespec::function();
			case type_string:
				return typespec::string_type();
			case type_weak:
				return typespec(type_kind::weak_ref);
			case type_typed_array:
				return typespec(type_kind::typed_array);
			case type_bool:
				return typespec::boolean();
			case type_nil:
				return typespec::nil();
			case type_number:
				return typespec::number();
			default:
				return typespec::any();
		}
	}

	bool bind(const typespec& pattern, const typespec& actual, std::vector<typespec>& bindings) {
		auto trial = bindings;
		if (!bind_impl(pattern, actual, trial))
			return false;
		bindings = std::move(trial);
		return true;
	}

	int specificity(const typespec& value) {
		auto child_score = [&]() {
			int score = 0;
			for (const typespec& child : value.children)
				score += std::min(specificity(child), 1000);
			return score;
		};

		switch (value.kind) {
			case type_kind::generic_param:
				return 0;
			case type_kind::any:
				return 1;
			case type_kind::union_type: {
				if (value.children.empty())
					return 2;
				int score = std::numeric_limits<int>::max();
				for (const typespec& child : value.children)
					score = std::min(score, specificity(child));
				return 2 + score;
			}
			case type_kind::optional:
			case type_kind::view:
			case type_kind::weak_ref:
			case type_kind::typed_array:
			case type_kind::fixed_array:
				return 20 + child_score();
			case type_kind::class_ref: {
				int depth = 0;
				for (const vclass* current = value.declared; current; current = current->super)
					++depth;
				return 100 + depth + child_score();
			}
			case type_kind::struct_ref:
				return 200 + child_score();
			case type_kind::function:
				return 50 + child_score();
			default:
				return 50;
		}
	}
}
