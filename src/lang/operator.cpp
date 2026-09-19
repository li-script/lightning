#include <array>
#include <cmath>
#include <lang/operator.hpp>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/traits.hpp>

namespace li {

	LI_COLD static any arg_error(vm* L, any a, const char* expected) {
		L->error("expected '%s', got '%s'", expected, type_names[a.type()]);
		return exception_marker;
	}

	static any retain_frame_result(vm* L, any value) {
		rc::retain_frame(L, value);
		return value;
	}

	static any invoke_binary_trait(vm* L, trait which, any self, any other) {
		std::array<any, 1> args = {other};
		return invoke_trait(L, which, self, args);
	}

	static any comparison_result(vm* L, any result, bool negate = false) {
		if (result.is_exc())
			return result;
		if (!result.is_bool()) [[unlikely]] {
			rc::release(L, result);
			L->error("comparison trait must return bool");
			return exception_marker;
		}
		if (!negate)
			return result;
		return any(!result.as_bool());
	}

	static any apply_trait_equality(vm* L, any lhs, any rhs, bool negate = false) {
		if (lhs.is_obj() && rhs.is_obj() && lhs.as_obj()->cl && rhs.as_obj()->cl && lhs.as_obj()->cl->value_semantics && rhs.as_obj()->cl->value_semantics &&
			 lhs.as_obj()->cl->identity == rhs.as_obj()->cl->identity)
			return any(object_equals(L, lhs.as_obj(), rhs.as_obj()) != negate);
		if (lhs == rhs)
			return any(!negate);
		if (has_trait(lhs, trait::eq))
			return comparison_result(L, invoke_binary_trait(L, trait::eq, lhs, rhs), negate);
		if (has_trait(rhs, trait::eq))
			return comparison_result(L, invoke_binary_trait(L, trait::eq, rhs, lhs), negate);
		return any(negate);
	}

	static any apply_trait_less(vm* L, any lhs, any rhs) {
		if (has_trait(lhs, trait::lt))
			return comparison_result(L, invoke_binary_trait(L, trait::lt, lhs, rhs));
		if (!lhs.is_num())
			return arg_error(L, lhs, type_names[type_number]);
		return arg_error(L, rhs, type_names[type_number]);
	}

	static any apply_trait_less_equal(vm* L, any lhs, any rhs) {
		if (!has_trait(lhs, trait::lt)) {
			if (!lhs.is_num())
				return arg_error(L, lhs, type_names[type_number]);
			return arg_error(L, rhs, type_names[type_number]);
		}

		any less = comparison_result(L, invoke_binary_trait(L, trait::lt, lhs, rhs));
		if (less.is_exc() || less.as_bool())
			return less;
		return apply_trait_equality(L, lhs, rhs);
	}

#define TYPE_ASSERT(v, t)       \
	if (!v.is<t>()) [[unlikely]] \
		return arg_error(L, v, type_names[t]);

	// Applies the unary/binary operator and returns an owned result. On failure,
	// returns the exception marker while vm::last_ex owns the error payload.
	//
	any apply_unary(vm* L, any a, bc::opcode op) {
		switch (op) {
			case bc::TOBOOL:
				return any(a.coerce_bool());
			case bc::LNOT:
				return any(!a.coerce_bool());
			case bc::ANEG: {
				if (a.is_num())
					return any(-a.as_num());
				if (has_trait(a, trait::neg))
					return invoke_trait(L, trait::neg, a);
				return arg_error(L, a, type_names[type_number]);
			}
			default:
				assume_unreachable();
		}
	}
	any apply_binary(vm* L, any a, any b, bc::opcode op) {
		switch (op) {
			case bc::NCS:
				return retain_frame_result(L, a == nil ? b : a);
			case bc::LOR:
				return retain_frame_result(L, a.coerce_bool() ? a : b);
			case bc::LAND:
				return retain_frame_result(L, a.coerce_bool() ? b : a);
			case bc::AADD: {
				if (a.is_num() && b.is_num())
					return any(a.as_num() + b.as_num());
				if (has_trait(a, trait::add))
					return invoke_binary_trait(L, trait::add, a, b);
				TYPE_ASSERT(a, type_number);
				return arg_error(L, b, type_names[type_number]);
			}
			case bc::ASUB: {
				if (a.is_num() && b.is_num())
					return any(a.as_num() - b.as_num());
				if (has_trait(a, trait::sub))
					return invoke_binary_trait(L, trait::sub, a, b);
				TYPE_ASSERT(a, type_number);
				return arg_error(L, b, type_names[type_number]);
			}
			case bc::AMUL: {
				if (a.is_num() && b.is_num())
					return any(a.as_num() * b.as_num());
				if (has_trait(a, trait::mul))
					return invoke_binary_trait(L, trait::mul, a, b);
				TYPE_ASSERT(a, type_number);
				return arg_error(L, b, type_names[type_number]);
			}
			case bc::ADIV: {
				if (a.is_num() && b.is_num())
					return any(a.as_num() / b.as_num());
				if (has_trait(a, trait::div))
					return invoke_binary_trait(L, trait::div, a, b);
				TYPE_ASSERT(a, type_number);
				return arg_error(L, b, type_names[type_number]);
			}
			case bc::AMOD: {
				if (a.is_num() && b.is_num()) {
					double x = a.as_num();
					double y = b.as_num();
#if LI_FAST_MATH
					return any(x - trunc(x / y) * y);
#else
					return any(fmod(x, y));
#endif
				}
				if (has_trait(a, trait::mod))
					return invoke_binary_trait(L, trait::mod, a, b);
				TYPE_ASSERT(a, type_number);
				return arg_error(L, b, type_names[type_number]);
			}
			case bc::APOW: {
				if (a.is_num() && b.is_num())
					return any(pow(a.as_num(), b.as_num()));
				if (has_trait(a, trait::pow))
					return invoke_binary_trait(L, trait::pow, a, b);
				TYPE_ASSERT(a, type_number);
				return arg_error(L, b, type_names[type_number]);
			}
			case bc::CEQ:
				return apply_trait_equality(L, a, b);
			case bc::CNE:
				return apply_trait_equality(L, a, b, true);
			case bc::CLT: {
				if (a.is_num() && b.is_num())
					return any(a.as_num() < b.as_num());
				return apply_trait_less(L, a, b);
			}
			case bc::CGT: {
				if (a.is_num() && b.is_num())
					return any(a.as_num() > b.as_num());
				return apply_trait_less(L, b, a);
			}
			case bc::CLE: {
				if (a.is_num() && b.is_num())
					return any(a.as_num() <= b.as_num());
				return apply_trait_less_equal(L, a, b);
			}
			case bc::CGE: {
				if (a.is_num() && b.is_num())
					return any(a.as_num() >= b.as_num());
				return apply_trait_less_equal(L, b, a);
			}
			default:
				assume_unreachable();
		}
	}
};