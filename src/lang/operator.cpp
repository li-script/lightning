#include <lang/operator.hpp>
#include <vm/array.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <lib/std.hpp>
#include <cmath>

namespace li {

	LI_COLD static any arg_error(vm* L, any a, const char* expected) {
		L->last_ex = string::format(L, "expected '%s', got '%s'", expected, type_names[a.type()]);
		return exception_marker;
	}

#define TYPE_ASSERT(v, t)     \
	if (!v.is<t>()) [[unlikely]] \
		return arg_error(L, v, type_names[t]);

	// Applies the unary/binary operator the values given. On failure (e.g. type mismatch),
	// returns the exception as the result.
	//
	LI_INLINE any apply_unary(vm* L, any a, bc::opcode op) {
		switch (op) {
			case bc::TOBOOL:
				return any(a.coerce_bool());
			case bc::LNOT:
				return any(!a.coerce_bool());
			case bc::ANEG: {
				TYPE_ASSERT(a, type_number);
				return any(-a.as_num());
			}
			default:
				assume_unreachable();
		}
	}
	LI_INLINE any apply_binary(vm* L, any a, any b, bc::opcode op) {
		switch (op) {
			case bc::NCS:
				return a == nil ? b : a;
			case bc::LOR:
				return a.coerce_bool() ? a : b;
			case bc::LAND:
				return a.coerce_bool() ? b : a;
			case bc::AADD:
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return any(a.as_num() + b.as_num());
			case bc::ASUB:
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return any(a.as_num() - b.as_num());
			case bc::AMUL:
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return any(a.as_num() * b.as_num());
			case bc::ADIV:
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return any(a.as_num() / b.as_num());
			case bc::AMOD: {
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);

				double x = a.as_num();
				double y = b.as_num();
				#if LI_FAST_MATH
				return any(x - trunc(x / y) * y);
				#else
				return any(fmod(x, y));
				#endif
			}
			case bc::APOW:
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return any(pow(a.as_num(), b.as_num()));
			case bc::CEQ:
				if (a == b)
					return true;
				return false;
			case bc::CNE:
				if (a == b)
					return false;
				return true;
			case bc::CLT:
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return any(a.as_num() < b.as_num());
			case bc::CGT:
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return any(a.as_num() > b.as_num());
			case bc::CLE:
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return any(a.as_num() <= b.as_num());
			case bc::CGE:
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return any(a.as_num() >= b.as_num());
			default:
				assume_unreachable();
		}
	}
};