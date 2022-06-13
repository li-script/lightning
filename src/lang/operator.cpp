#include <lang/operator.hpp>
#include <vm/array.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <lib/std.hpp>
#include <cmath>

namespace li {

	LI_COLD static any arg_error(vm* L, any a, const char* expected)
	{
		L->last_ex = string::format(L, "expected '%s', got '%s'", expected, type_names[a.type()]);
		return exception_marker;
	}
#define TYPE_ASSERT(v, t)     \
	if (!v.is(t)) [[unlikely]] \
		return arg_error(L, v, type_names[t]);

#define UNARY_APPLY_TRAIT(t)                           \
	if (a.is_traitful()) [[unlikely]] {                 \
		auto* ta = (traitful_node<>*) a.as_gc();         \
		if (ta->has_trait<t>()) {                        \
			return L->call(0, ta->get_trait<t>(), a);     \
		}                                                \
	}
#define BINARY_APPLY_TRAIT_AND(t, TF)                                                \
	if (a.is_traitful() || b.is_traitful()) [[unlikely]]                              \
		do {                                                                           \
			trait_pointer tp = {};                                                      \
			any           self;                                                         \
			if (a.is_traitful() && ((traitful_node<>*) a.as_gc())->has_trait<t>())      \
				tp = ((traitful_node<>*) a.as_gc())->traits->list[msize_t(t)], self = a; \
			else if (b.is_traitful() && ((traitful_node<>*) b.as_gc())->has_trait<t>()) \
				tp = ((traitful_node<>*) b.as_gc())->traits->list[msize_t(t)], self = b; \
			if (tp.pointer) {                                                           \
				L->push_stack(b);                                                        \
				L->push_stack(a);                                                        \
				any res = L->call(2, tp.as_any(), self);                                 \
				return TF(res);                                                          \
			}                                                                           \
		} while (0);
#define BINARY_APPLY_TRAIT(t) BINARY_APPLY_TRAIT_AND(t, LI_IDENTITY)

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
				UNARY_APPLY_TRAIT(trait::neg);
				TYPE_ASSERT(a, type_number);
				return any(-a.as_num());
			}
			default:
				assume_unreachable();
		}
	}
	LI_INLINE any apply_binary(vm* L, any a, any b, bc::opcode op) {
		switch (op) {
			case bc::VIN: {
				any tmp[] = {a, b};
				return any(std::in_place, lib::detail::builtin_in_info.invoke(L, tmp, 1));
			}
			case bc::NCS:
				return a == nil ? b : a;
			case bc::LOR:
				return a.coerce_bool() ? a : b;
			case bc::LAND:
				return a.coerce_bool() ? b : a;
			case bc::AADD:
				BINARY_APPLY_TRAIT(trait::add);
				if (a.is_arr()) {
					a.as_arr()->push(L, b);
					return any(a);
				} else if (a.is_str()) {
					TYPE_ASSERT(b, type_string);
					return any(string::concat(L, a.as_str(), b.as_str()));
				} else {
					TYPE_ASSERT(a, type_number);
					TYPE_ASSERT(b, type_number);
					return any(a.as_num() + b.as_num());
				}
			case bc::ASUB:
				BINARY_APPLY_TRAIT(trait::sub);
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return any(a.as_num() - b.as_num());
			case bc::AMUL:
				BINARY_APPLY_TRAIT(trait::mul);
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return any(a.as_num() * b.as_num());
			case bc::ADIV:
				BINARY_APPLY_TRAIT(trait::div);
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return any(a.as_num() / b.as_num());
			case bc::AMOD: {
				BINARY_APPLY_TRAIT(trait::mod);
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
				BINARY_APPLY_TRAIT(trait::pow);
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return any(pow(a.as_num(), b.as_num()));
			case bc::CEQ:
				if (a == b)
					return true;
				BINARY_APPLY_TRAIT(trait::eq);
				return false;
			case bc::CNE:
				if (a == b)
					return false;
				#define NEGATE(res) (!res.is_exc())?any(!res.as_bool()):res
				BINARY_APPLY_TRAIT_AND(trait::eq, NEGATE);
				#undef NEGATE
				return true;
			case bc::CLT:
				BINARY_APPLY_TRAIT(trait::lt);
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return any(a.as_num() < b.as_num());
			case bc::CGT:
				{
					auto x=a,y=b;
					auto a=y,b=x;
					BINARY_APPLY_TRAIT(trait::lt);
				}
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return any(a.as_num() > b.as_num());
			case bc::CLE:
				BINARY_APPLY_TRAIT(trait::le);
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return any(a.as_num() <= b.as_num());
			case bc::CGE:
				{
					auto x=a,y=b;
					auto a=y,b=x;
					BINARY_APPLY_TRAIT(trait::le);
				}
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return any(a.as_num() >= b.as_num());
			default:
				assume_unreachable();
		}
	}
};