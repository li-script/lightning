#include <lang/operator.hpp>
#include <vm/array.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <cmath>

namespace li {

	LI_COLD static any arg_error(vm* L, any a, const char* expected) { return string::format(L, "expected '%s', got '%s'", expected, type_names[a.type()]); }
#define TYPE_ASSERT(v, t)     \
	if (!v.is(t)) [[unlikely]] \
		return {arg_error(L, v, type_names[t]), false};

	// TODO: Coerce + Meta

	// Applies the unary/binary operator the values given. On failure (e.g. type mismatch),
	// returns the exception as the result and false for the second return.
	//
	LI_INLINE std::pair<any, bool> apply_unary(vm* L, any a, bc::opcode op) {
		switch (op) {
			case bc::LNOT: {
				return {any(!a.as_bool()), true};
			}
			case bc::ANEG: {
				TYPE_ASSERT(a, type_number);
				return {any(-a.as_num()), true};
			}
			default:
				assume_unreachable();
		}
	}
	LI_INLINE std::pair<any, bool> apply_binary(vm* L, any a, any b, bc::opcode op) {
		switch (op) {
			case bc::AADD:
				if (a.is_arr()) {
					a.as_arr()->push(L, b);
					return {any(a), true};
				} else if (a.is_str()) {
					TYPE_ASSERT(b, type_string);
					return {any(string::concat(L, a.as_str(), b.as_str())), true};
				} else {
					TYPE_ASSERT(a, type_number);
					TYPE_ASSERT(b, type_number);
					return {any(a.as_num() + b.as_num()), true};
				}
			case bc::ASUB:
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return {any(a.as_num() - b.as_num()), true};
			case bc::AMUL:
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return {any(a.as_num() * b.as_num()), true};
			case bc::ADIV:
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return {any(a.as_num() / b.as_num()), true};
			case bc::AMOD:
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return {any(fmod(a.as_num(), b.as_num())), true};
			case bc::APOW:
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return {any(pow(a.as_num(), b.as_num())), true};
			case bc::LAND:
				return {any(bool(a.as_bool() & b.as_bool())), true};
			case bc::NCS:
				return {a.is(type_none) ? b : a, true};
			case bc::LOR:
				return {any(bool(a.as_bool() | b.as_bool())), true};
			case bc::CEQ:
				return {any(a == b), true};
			case bc::CNE:
				return {any(a != b), true};
			case bc::CLT:
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return {any(a.as_num() < b.as_num()), true};
			case bc::CGT:
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return {any(a.as_num() > b.as_num()), true};
			case bc::CLE:
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return {any(a.as_num() <= b.as_num()), true};
			case bc::CGE:
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return {any(a.as_num() >= b.as_num()), true};
			default:
				assume_unreachable();
		}
	}
};