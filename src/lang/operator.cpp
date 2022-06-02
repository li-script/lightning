#include <lang/operator.hpp>
#include <vm/array.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <cmath>

namespace li {

	LI_COLD static any arg_error(vm* L, any a, const char* expected) { return string::format(L, "expected '%s', got '%s'", expected, type_names[a.type()]); }
#define TYPE_ASSERT(v, t)     \
	if (!v.is(t)) [[unlikely]] \
		return {arg_error(L, v, type_names[t]), false};

#define UNARY_APPLY_TRAIT(t)                           \
	if (a.is_traitful()) [[unlikely]] {                 \
		auto* ta = (traitful_node<>*) a.as_gc();         \
		if (ta->has_trait<t>()) {                        \
			bool ok = L->scall(0, ta->get_trait<t>(), a); \
			return {L->pop_stack(), ok};                  \
		}                                                \
	}
#define BINARY_APPLY_TRAIT_AND(t, TF)                                                 \
	if (a.is_traitful() || b.is_traitful()) [[unlikely]]                               \
		do {                                                                            \
			trait_pointer tp = {};                                                       \
			any           self;                                                          \
			if (a.is_traitful() && ((traitful_node<>*) a.as_gc())->has_trait<t>())       \
				tp = ((traitful_node<>*) a.as_gc())->traits->list[uint32_t(t)], self = a; \
			else if (b.is_traitful() && ((traitful_node<>*) b.as_gc())->has_trait<t>())  \
				tp = ((traitful_node<>*) b.as_gc())->traits->list[uint32_t(t)], self = b; \
			if (tp.pointer) {                                                            \
				L->push_stack(b);                                                         \
				L->push_stack(a);                                                         \
				bool ok     = L->scall(2, tp.as_any(), self);                             \
				any  retval = L->pop_stack();                                             \
				return {TF(retval, ok)};                                                  \
			}                                                                            \
		} while (0);
#define BINARY_APPLY_TRAIT(t) BINARY_APPLY_TRAIT_AND(t, LI_IDENTITY)

	// Applies the unary/binary operator the values given. On failure (e.g. type mismatch),
	// returns the exception as the result and false for the second return.
	//
	LI_INLINE std::pair<any, bool> apply_unary(vm* L, any a, bc::opcode op) {
		switch (op) {
			case bc::LNOT: {
				return {any(!a.coerce_bool()), true};
			}
			case bc::ANEG: {
				UNARY_APPLY_TRAIT(trait::neg);
				TYPE_ASSERT(a, type_number);
				return {any(-a.as_num()), true};
			}
			case bc::VLEN: {
				UNARY_APPLY_TRAIT(trait::len);
				if (a.is_arr()) {
					return {any(number(a.as_arr()->length)), true};
				} else if (a.is_tbl()) {
					return {any(number(a.as_tbl()->active_count)), true};
				} else if (a.is_str()) {
					return {any(number(a.as_str()->length)), true};
				} else [[unlikely]] {
					return {arg_error(L, a, "iterable"), false};
				}
			}

			default:
				assume_unreachable();
		}
	}
	LI_INLINE std::pair<any, bool> apply_binary(vm* L, any a, any b, bc::opcode op) {
		switch (op) {
			case bc::VIN: {
				if (b.is_arr()) {
					for (auto& k : *b.as_arr())
						if (k == a)
							return {const_true, true};
					return {const_false, true};
				} else if (b.is_tbl()) {
					return {any(a != none && b.as_tbl()->get(L, a) != none), true};
				} else if (b.is_str()) {

					if (a.is_num()) {
						if (auto num = uint32_t(a.as_num()); num <= 0xFF) {
							for (auto& k : b.as_str()->view())
								if (uint8_t(k) == num)
									return {const_true, true};
						}
						return {const_false, true};
					} else if (a.is_str()) {
						return { bool(b.as_str()->view().find(a.as_str()->view())!=std::string::npos), true };
					} else {
						return {arg_error(L, b, "string or character"), false};
					}
				} else [[unlikely]] {
					return {arg_error(L, b, "iterable"), false};
				}
			}
			case bc::NCS:
				return {a == none ? b : a, true};
			case bc::LOR:
				return {a.coerce_bool() ? a : b, true};
			case bc::LAND:
				return {a.coerce_bool() ? b : a, true};
			case bc::AADD:
				BINARY_APPLY_TRAIT(trait::add);
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
				BINARY_APPLY_TRAIT(trait::sub);
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return {any(a.as_num() - b.as_num()), true};
			case bc::AMUL:
				BINARY_APPLY_TRAIT(trait::mul);
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return {any(a.as_num() * b.as_num()), true};
			case bc::ADIV:
				BINARY_APPLY_TRAIT(trait::div);
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return {any(a.as_num() / b.as_num()), true};
			case bc::AMOD:
				BINARY_APPLY_TRAIT(trait::mod);
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return {any(fmod(a.as_num(), b.as_num())), true};
			case bc::APOW:
				BINARY_APPLY_TRAIT(trait::pow);
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return {any(pow(a.as_num(), b.as_num())), true};
			case bc::CEQ:
				if (a == b)
					return {true, true};
				BINARY_APPLY_TRAIT(trait::eq);
				return {false, true};
			case bc::CNE:
				if (a == b)
					return {false, true};
				#define NEGATE(res, ok) ok?any(!res.as_bool()):res, ok
				BINARY_APPLY_TRAIT_AND(trait::eq, NEGATE);
				#undef NEGATE
				return {true, true};
			case bc::CLT:
				BINARY_APPLY_TRAIT(trait::lt);
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return {any(a.as_num() < b.as_num()), true};
			case bc::CGT:
				{
					auto x=a,y=b;
					auto a=y,b=x;
					BINARY_APPLY_TRAIT(trait::lt);
				}
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return {any(a.as_num() > b.as_num()), true};
			case bc::CLE:
				BINARY_APPLY_TRAIT(trait::le);
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return {any(a.as_num() <= b.as_num()), true};
			case bc::CGE:
				{
					auto x=a,y=b;
					auto a=y,b=x;
					BINARY_APPLY_TRAIT(trait::le);
				}
				TYPE_ASSERT(a, type_number);
				TYPE_ASSERT(b, type_number);
				return {any(a.as_num() >= b.as_num()), true};
			default:
				assume_unreachable();
		}
	}
};