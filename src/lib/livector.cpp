#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <lib/std.hpp>
#include <span>
#include <string_view>
#include <util/user.hpp>
#include <vector>
#include <vm/function.hpp>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/shared.hpp>
#include <vm/string.hpp>
#include <vm/traits.hpp>

namespace li::lib {
	namespace {
		constexpr std::string_view vector_dimension_key = "@vector-dimension";

		struct vector_snapshot {
			object*              instance  = nullptr;
			std::size_t          dimension = 0;
			std::array<float, 4> components{};
		};

		std::size_t vector_dimension(vclass* cl) {
			if (!cl)
				return 0;
			for (const field_pair& field : cl->fields()) {
				if (!field.value.is_static || field.value.ty != type::i32 || field.key->view() != vector_dimension_key)
					continue;
				int32_t dimension = 0;
				std::memcpy(&dimension, cl->static_space() + field.value.offset, sizeof(dimension));
				return dimension >= 2 && dimension <= 4 ? std::size_t(dimension) : 0;
			}
			return 0;
		}

		bool read_vector(vm* L, any_t value, vector_snapshot& result, const char* operation) {
			if (!value.is_obj()) {
				L->error("%s expects a vector", operation);
				return false;
			}
			object*     instance  = value.as_obj();
			std::size_t dimension = vector_dimension(instance->cl);
			if (!dimension) {
				L->error("%s expects a vector", operation);
				return false;
			}

			shared::recursive_guard guard(L, instance);
			result.instance  = instance;
			result.dimension = dimension;
			for (std::size_t index = 0; index != dimension; ++index)
				std::memcpy(&result.components[index], instance->data + index * sizeof(float), sizeof(float));
			return true;
		}

		bool read_same_vector(vm* L, const vector_snapshot& lhs, any_t value, vector_snapshot& rhs, const char* operation) {
			if (!read_vector(L, value, rhs, operation))
				return false;
			if (lhs.instance->cl->identity != rhs.instance->cl->identity) {
				L->error("%s expects vectors of the same type", operation);
				return false;
			}
			return true;
		}

		any_t make_vector(vm* L, vclass* cl, std::span<const double> components, const char* operation) {
			const std::size_t dimension = vector_dimension(cl);
			if (!dimension || components.size() != dimension)
				return L->error("%s received an invalid vector class", operation);

			object* result = object::create(L, cl);
			for (std::size_t index = 0; index != dimension; ++index) {
				const float rounded = static_cast<float>(components[index]);
				std::memcpy(result->data + index * sizeof(float), &rounded, sizeof(rounded));
			}
			return L->take(any(result));
		}

		any_t LI_CC vector_new2(vm* L, any_t self, double x, double y) {
			const std::array<double, 2> components{x, y};
			return make_vector(L, self.is_vcl() ? self.as_vcl() : nullptr, components, "vector constructor");
		}

		any_t LI_CC vector_new3(vm* L, any_t self, double x, double y, double z) {
			const std::array<double, 3> components{x, y, z};
			return make_vector(L, self.is_vcl() ? self.as_vcl() : nullptr, components, "vector constructor");
		}

		any_t LI_CC vector_new4(vm* L, any_t self, double x, double y, double z, double w) {
			const std::array<double, 4> components{x, y, z, w};
			return make_vector(L, self.is_vcl() ? self.as_vcl() : nullptr, components, "vector constructor");
		}

		any_t vector_new_vm(vm* L, any* args, slot_t count) {
			if (!args[1].is_vcl())
				return L->error("vector constructor requires a vector class");
			const std::size_t dimension = vector_dimension(args[1].as_vcl());
			if (!dimension || count != slot_t(dimension))
				return L->error("vec%zu::new expects %zu numbers", dimension, dimension);

			std::array<double, 4> components{};
			for (std::size_t index = 0; index != dimension; ++index) {
				any_t value = args[-slot_t(index)];
				if (!value.is_num())
					return L->error("vec%zu::new expects %zu numbers", dimension, dimension);
				components[index] = value.as_num();
			}
			return make_vector(L, args[1].as_vcl(), std::span<const double>{components.data(), dimension}, "vector constructor");
		}

		enum class binary_operation : uint8_t {
			add,
			sub,
			mul,
			div,
		};

		const char* operation_name(binary_operation operation) {
			switch (operation) {
				case binary_operation::add:
					return "vector addition";
				case binary_operation::sub:
					return "vector subtraction";
				case binary_operation::mul:
					return "vector multiplication";
				case binary_operation::div:
					return "vector division";
			}
			assume_unreachable();
		}

		double apply(binary_operation operation, double lhs, double rhs) {
			switch (operation) {
				case binary_operation::add:
					return lhs + rhs;
				case binary_operation::sub:
					return lhs - rhs;
				case binary_operation::mul:
					return lhs * rhs;
				case binary_operation::div:
					return lhs / rhs;
			}
			assume_unreachable();
		}

		any_t vector_binary_scalar(vm* L, any_t self, double rhs, binary_operation operation) {
			vector_snapshot lhs;
			if (!read_vector(L, self, lhs, operation_name(operation)))
				return exception_marker;

			std::array<double, 4> result{};
			for (std::size_t index = 0; index != lhs.dimension; ++index)
				result[index] = apply(operation, lhs.components[index], rhs);
			return make_vector(L, lhs.instance->cl, std::span<const double>{result.data(), lhs.dimension}, operation_name(operation));
		}

		any_t vector_binary_vector(vm* L, any_t self, any_t other, binary_operation operation) {
			vector_snapshot lhs;
			vector_snapshot rhs;
			if (!read_vector(L, self, lhs, operation_name(operation)) || !read_same_vector(L, lhs, other, rhs, operation_name(operation)))
				return exception_marker;

			std::array<double, 4> result{};
			for (std::size_t index = 0; index != lhs.dimension; ++index)
				result[index] = apply(operation, lhs.components[index], rhs.components[index]);
			return make_vector(L, lhs.instance->cl, std::span<const double>{result.data(), lhs.dimension}, operation_name(operation));
		}

		any_t vector_binary_vm(vm* L, any* args, slot_t count, binary_operation operation) {
			if (count != 1)
				return L->error("%s expects one scalar or vector operand", operation_name(operation));
			if (args[0].is_num())
				return vector_binary_scalar(L, args[1], args[0].as_num(), operation);
			return vector_binary_vector(L, args[1], args[0], operation);
		}

#define LI_VECTOR_BINARY(NAME, OPERATION)                                                                                                        \
	any_t LI_CC vector_##NAME##_scalar(vm* L, any_t self, double rhs) { return vector_binary_scalar(L, self, rhs, binary_operation::OPERATION); } \
	any_t LI_CC vector_##NAME##_vector(vm* L, any_t self, any_t rhs) { return vector_binary_vector(L, self, rhs, binary_operation::OPERATION); }  \
	any_t       vector_##NAME##_vm(vm* L, any* args, slot_t count) { return vector_binary_vm(L, args, count, binary_operation::OPERATION); }

		LI_VECTOR_BINARY(add, add)
		LI_VECTOR_BINARY(sub, sub)
		LI_VECTOR_BINARY(mul, mul)
		LI_VECTOR_BINARY(div, div)
#undef LI_VECTOR_BINARY

		any_t LI_CC vector_neg_typed(vm* L, any_t self) {
			vector_snapshot value;
			if (!read_vector(L, self, value, "vector negation"))
				return exception_marker;
			std::array<double, 4> result{};
			for (std::size_t index = 0; index != value.dimension; ++index)
				result[index] = -double(value.components[index]);
			return make_vector(L, value.instance->cl, std::span<const double>{result.data(), value.dimension}, "vector negation");
		}

		any_t vector_neg_vm(vm* L, any* args, slot_t count) {
			if (count != 0)
				return L->error("vector negation expects no arguments");
			return vector_neg_typed(L, args[1]);
		}

		uint8_t LI_CC vector_eq_typed(any_t self, any_t other) {
			if (!self.is_obj() || !other.is_obj())
				return false;
			object*           lhs       = self.as_obj();
			object*           rhs       = other.as_obj();
			const std::size_t dimension = vector_dimension(lhs->cl);
			if (!dimension || lhs->cl->identity != rhs->cl->identity)
				return false;

			std::array<float, 4> lhs_components{};
			std::array<float, 4> rhs_components{};
			{
				shared::recursive_guard guard(nullptr, lhs);
				for (std::size_t index = 0; index != dimension; ++index)
					std::memcpy(&lhs_components[index], lhs->data + index * sizeof(float), sizeof(float));
			}
			{
				shared::recursive_guard guard(nullptr, rhs);
				for (std::size_t index = 0; index != dimension; ++index)
					std::memcpy(&rhs_components[index], rhs->data + index * sizeof(float), sizeof(float));
			}
			for (std::size_t index = 0; index != dimension; ++index) {
				if (lhs_components[index] != rhs_components[index])
					return false;
			}
			return true;
		}

		any_t vector_eq_vm(vm* L, any* args, slot_t count) {
			if (count != 1)
				return L->error("vector equality expects one operand");
			return L->ok(any(bool(vector_eq_typed(args[1], args[0]))));
		}

		any_t LI_CC vector_str_typed(vm* L, any_t self) {
			vector_snapshot value;
			if (!read_vector(L, self, value, "vector string conversion"))
				return exception_marker;
			switch (value.dimension) {
				case 2:
					return L->take(any(string::format(L, "vec2(%.9g, %.9g)", double(value.components[0]), double(value.components[1]))));
				case 3:
					return L->take(
						 any(string::format(L, "vec3(%.9g, %.9g, %.9g)", double(value.components[0]), double(value.components[1]), double(value.components[2]))));
				case 4:
					return L->take(any(string::format(L, "vec4(%.9g, %.9g, %.9g, %.9g)", double(value.components[0]), double(value.components[1]),
						 double(value.components[2]), double(value.components[3]))));
				default:
					assume_unreachable();
			}
		}

		any_t vector_str_vm(vm* L, any* args, slot_t count) {
			if (count != 0)
				return L->error("vector string conversion expects no arguments");
			return vector_str_typed(L, args[1]);
		}

		any_t LI_CC vector_dot_typed(vm* L, any_t self, any_t other) {
			vector_snapshot lhs;
			vector_snapshot rhs;
			if (!read_vector(L, self, lhs, "vector dot") || !read_same_vector(L, lhs, other, rhs, "vector dot"))
				return exception_marker;
			double result = 0;
			for (std::size_t index = 0; index != lhs.dimension; ++index)
				result += double(lhs.components[index]) * double(rhs.components[index]);
			return L->ok(any(result));
		}

		any_t vector_dot_vm(vm* L, any* args, slot_t count) {
			if (count != 1)
				return L->error("vector dot expects one vector operand");
			return vector_dot_typed(L, args[1], args[0]);
		}

		any_t LI_CC vector_cross_typed(vm* L, any_t self, any_t other) {
			vector_snapshot lhs;
			vector_snapshot rhs;
			if (!read_vector(L, self, lhs, "vector cross") || !read_same_vector(L, lhs, other, rhs, "vector cross"))
				return exception_marker;
			if (lhs.dimension != 3)
				return L->error("vector cross expects vec3 operands");
			const std::array<double, 3> result{
				 double(lhs.components[1]) * double(rhs.components[2]) - double(lhs.components[2]) * double(rhs.components[1]),
				 double(lhs.components[2]) * double(rhs.components[0]) - double(lhs.components[0]) * double(rhs.components[2]),
				 double(lhs.components[0]) * double(rhs.components[1]) - double(lhs.components[1]) * double(rhs.components[0]),
			};
			return make_vector(L, lhs.instance->cl, result, "vector cross");
		}

		any_t vector_cross_vm(vm* L, any* args, slot_t count) {
			if (count != 1)
				return L->error("vector cross expects one vec3 operand");
			return vector_cross_typed(L, args[1], args[0]);
		}

		any_t LI_CC vector_length_typed(vm* L, any_t self) {
			vector_snapshot value;
			if (!read_vector(L, self, value, "vector length"))
				return exception_marker;
			double squared = 0;
			for (std::size_t index = 0; index != value.dimension; ++index)
				squared += double(value.components[index]) * double(value.components[index]);
			return L->ok(any(std::sqrt(squared)));
		}

		any_t vector_length_vm(vm* L, any* args, slot_t count) {
			if (count != 0)
				return L->error("vector length expects no arguments");
			return vector_length_typed(L, args[1]);
		}

		any_t LI_CC vector_normalize_typed(vm* L, any_t self) {
			vector_snapshot value;
			if (!read_vector(L, self, value, "vector normalize"))
				return exception_marker;
			double squared = 0;
			for (std::size_t index = 0; index != value.dimension; ++index)
				squared += double(value.components[index]) * double(value.components[index]);
			const double          length = std::sqrt(squared);
			std::array<double, 4> result{};
			for (std::size_t index = 0; index != value.dimension; ++index)
				result[index] = double(value.components[index]) / length;
			return make_vector(L, value.instance->cl, std::span<const double>{result.data(), value.dimension}, "vector normalize");
		}

		any_t vector_normalize_vm(vm* L, any* args, slot_t count) {
			if (count != 0)
				return L->error("vector normalize expects no arguments");
			return vector_normalize_typed(L, args[1]);
		}

		any_t LI_CC vector_lerp_typed(vm* L, any_t self, any_t other, double amount) {
			vector_snapshot lhs;
			vector_snapshot rhs;
			if (!read_vector(L, self, lhs, "vector lerp") || !read_same_vector(L, lhs, other, rhs, "vector lerp"))
				return exception_marker;
			std::array<double, 4> result{};
			for (std::size_t index = 0; index != lhs.dimension; ++index)
				result[index] = double(lhs.components[index]) + (double(rhs.components[index]) - double(lhs.components[index])) * amount;
			return make_vector(L, lhs.instance->cl, std::span<const double>{result.data(), lhs.dimension}, "vector lerp");
		}

		any_t vector_lerp_vm(vm* L, any* args, slot_t count) {
			if (count != 2 || !args[-1].is_num())
				return L->error("vector lerp expects a vector and number");
			return vector_lerp_typed(L, args[1], args[0], args[-1].as_num());
		}

		util::native_function vector_new = {
			 func_attr_sideeffect | func_attr_c_takes_vm | func_attr_c_takes_self,
			 "vector.new",
			 &vector_new_vm,
			 {
				  nfunc_overload{li::bit_cast<const void*>(&vector_new2), {type::any, type::f64, type::f64}, type::any},
				  nfunc_overload{li::bit_cast<const void*>(&vector_new3), {type::any, type::f64, type::f64, type::f64}, type::any},
				  nfunc_overload{li::bit_cast<const void*>(&vector_new4), {type::any, type::f64, type::f64, type::f64, type::f64}, type::any},
			 },
		};

#define LI_VECTOR_BINARY_FUNCTION(NAME)                                                                           \
	util::native_function vector_##NAME = {                                                                        \
		 func_attr_sideeffect | func_attr_c_takes_vm | func_attr_c_takes_self,                                      \
		 "vector." #NAME,                                                                                           \
		 &vector_##NAME##_vm,                                                                                       \
		 {                                                                                                          \
			  nfunc_overload{li::bit_cast<const void*>(&vector_##NAME##_scalar), {type::any, type::f64}, type::any}, \
			  nfunc_overload{li::bit_cast<const void*>(&vector_##NAME##_vector), {type::any, type::any}, type::any}, \
		 },                                                                                                         \
	};

		LI_VECTOR_BINARY_FUNCTION(add)
		LI_VECTOR_BINARY_FUNCTION(sub)
		LI_VECTOR_BINARY_FUNCTION(mul)
		LI_VECTOR_BINARY_FUNCTION(div)
#undef LI_VECTOR_BINARY_FUNCTION

		util::native_function vector_neg = {
			 func_attr_sideeffect | func_attr_c_takes_vm | func_attr_c_takes_self,
			 "vector.neg",
			 &vector_neg_vm,
			 {nfunc_overload{li::bit_cast<const void*>(&vector_neg_typed), {type::any}, type::any}},
		};
		util::native_function vector_eq = {
			 func_attr_sideeffect | func_attr_c_takes_self,
			 "vector.eq",
			 &vector_eq_vm,
			 {nfunc_overload{li::bit_cast<const void*>(&vector_eq_typed), {type::any, type::any}, type::i1}},
		};
		util::native_function vector_str = {
			 func_attr_sideeffect | func_attr_c_takes_vm | func_attr_c_takes_self,
			 "vector.str",
			 &vector_str_vm,
			 {nfunc_overload{li::bit_cast<const void*>(&vector_str_typed), {type::any}, type::any}},
		};
		util::native_function vector_dot = {
			 func_attr_sideeffect | func_attr_c_takes_vm | func_attr_c_takes_self,
			 "vector.dot",
			 &vector_dot_vm,
			 {nfunc_overload{li::bit_cast<const void*>(&vector_dot_typed), {type::any, type::any}, type::any}},
		};
		util::native_function vector_cross = {
			 func_attr_sideeffect | func_attr_c_takes_vm | func_attr_c_takes_self,
			 "vector.cross",
			 &vector_cross_vm,
			 {nfunc_overload{li::bit_cast<const void*>(&vector_cross_typed), {type::any, type::any}, type::any}},
		};
		util::native_function vector_length = {
			 func_attr_sideeffect | func_attr_c_takes_vm | func_attr_c_takes_self,
			 "vector.length",
			 &vector_length_vm,
			 {nfunc_overload{li::bit_cast<const void*>(&vector_length_typed), {type::any}, type::any}},
		};
		util::native_function vector_normalize = {
			 func_attr_sideeffect | func_attr_c_takes_vm | func_attr_c_takes_self,
			 "vector.normalize",
			 &vector_normalize_vm,
			 {nfunc_overload{li::bit_cast<const void*>(&vector_normalize_typed), {type::any}, type::any}},
		};
		util::native_function vector_lerp = {
			 func_attr_sideeffect | func_attr_c_takes_vm | func_attr_c_takes_self,
			 "vector.lerp",
			 &vector_lerp_vm,
			 {nfunc_overload{li::bit_cast<const void*>(&vector_lerp_typed), {type::any, type::any, type::f64}, type::any}},
		};

		struct named_method {
			const char* name;
			function*   method;
		};

		vclass* create_vector_class(vm* L, std::size_t dimension) {
			static constexpr std::array<const char*, 4> component_names{"x", "y", "z", "w"};
			const std::array<named_method, 5>           methods{
				 named_method{"dot", &vector_dot},
				 named_method{"cross", &vector_cross},
				 named_method{"length", &vector_length},
				 named_method{"normalize", &vector_normalize},
				 named_method{"lerp", &vector_lerp},
			};

			const std::size_t     method_count          = dimension == 3 ? methods.size() : methods.size() - 1;
			constexpr std::size_t function_storage_size = size_of_data(type::fn);
			std::vector<string*>  keys;
			keys.reserve(dimension + method_count + 1);
			std::vector<field_pair> fields;
			fields.reserve(dimension + method_count + 1);
			for (std::size_t index = 0; index != dimension; ++index) {
				string* key = string::create(L, component_names[index]);
				keys.push_back(key);
				fields.push_back(field_pair{
					 .key   = key,
					 .value = field_info{.ty = type::f32, .offset = uint32_t(index * sizeof(float))},
				});
			}

			std::vector<uint8_t> static_values(method_count * function_storage_size + sizeof(int32_t), 0);
			std::size_t          static_index = 0;
			for (const named_method& method : methods) {
				if (dimension != 3 && std::string_view(method.name) == "cross")
					continue;
				string* key = string::create(L, method.name);
				keys.push_back(key);
				fields.push_back(field_pair{
					 .key   = key,
					 .value = field_info{.ty = type::fn, .offset = uint32_t(static_index * function_storage_size), .is_static = true},
				});
				any(method.method).store_at(static_values.data() + static_index * function_storage_size, type::fn);
				++static_index;
			}

			string* dimension_key = string::create(L, vector_dimension_key);
			keys.push_back(dimension_key);
			const uint32_t dimension_offset = uint32_t(method_count * function_storage_size);
			fields.push_back(field_pair{
				 .key   = dimension_key,
				 .value = field_info{.ty = type::i32, .offset = dimension_offset, .is_static = true},
			});
			const int32_t stored_dimension = int32_t(dimension);
			std::memcpy(static_values.data() + dimension_offset, &stored_dimension, sizeof(stored_dimension));

			std::vector<uint8_t> defaults(dimension * sizeof(float), 0);
			string*              class_name = string::format(L, "vec%zu", dimension);
			vclass*              result     = vclass::create(L, class_name, fields, defaults, static_values, nullptr, reserve_class_identity(L));
			result->set_ctor(L, &vector_new);
			result->traits                              = L->alloc<trait_set>();
			result->traits->methods[size_t(trait::add)] = &vector_add;
			result->traits->methods[size_t(trait::sub)] = &vector_sub;
			result->traits->methods[size_t(trait::mul)] = &vector_mul;
			result->traits->methods[size_t(trait::div)] = &vector_div;
			result->traits->methods[size_t(trait::neg)] = &vector_neg;
			result->traits->methods[size_t(trait::eq)]  = &vector_eq;
			result->traits->methods[size_t(trait::str)] = &vector_str;
			for (function* method : result->traits->methods)
				rc::retain(method);

			rc::release(L, class_name);
			for (string* key : keys)
				rc::release(L, key);
			return result;
		}
	}

	void register_vectors(vm* L) {
		for (std::size_t dimension = 2; dimension <= 4; ++dimension) {
			vclass* value  = create_vector_class(L, dimension);
			char    name[] = {'v', 'e', 'c', char('0' + dimension), '\0'};
			util::export_as(L, name, any(value));
			rc::release(L, value);
		}
	}
}
