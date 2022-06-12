#include <lib/std.hpp>
#include <util/user.hpp>
#include <lang/parser.hpp>
#include <cmath>
#include <bit>
#include <numbers>

namespace li::lib {
	static uint64_t math_random_to_dbl(vm* L, any* args, slot_t n, uint64_t v) {
		constexpr uint32_t mantissa_bits = 52;
		constexpr uint32_t exponent_bits = 11;
		constexpr uint32_t exponent_0    = uint32_t((1u << (exponent_bits - 1)) - 3);  // 2^-2, max at 0.499999.

		// Find the lowest bit set, 0 requires 1 bit to be set to '0', 1 requires
		// 2 bits to be set to '0', each increment has half the chance of being
		// returned compared to the previous one which leads to equal distribution
		// between exponential levels.
		//
		constexpr uint32_t exponent_seed_bits = 64 - (mantissa_bits + 1);
		uint32_t           exponent           = exponent_0 - std::countr_zero<uint32_t>(uint32_t(v) | (1u << exponent_seed_bits));

		// Clear and replace the exponent bits.
		//
		v &= ~((1ull << exponent_bits) - 1);
		v |= exponent;

		// Rotate until mantissa is at the bottom, add 0.5 to shift from [-0.5, +0.5] to [0.0, 1.0].
		//
		v        = std::rotl(v, mantissa_bits);
		double r = li::bit_cast<double>(v) + 0.5;

		// If no args given, return.
		//
		if (n == 0) {
			return L->ok(r);
		}

		// Validate args.
		//
		if (n != 1 && n != 2) {
			return L->error("expected one or two numbers.");
		}

		// If one arg given, generate [0, x] inclusive.
		//
		if (!args[1 - n].is_num())
			return L->error("expected one or two numbers.");
		double x = args[1 - n].as_num();

		// If two args given, generate [y, x] inclusive.
		//
		double y = 0;
		if (n == 2) {
			if (!args[0].is_num())
				return L->error("expected one or two numbers.");
			y = args[0].as_num();
		}
		return L->ok(y + r * (x - y));
	}
	static uint64_t math_random(vm* L, any* args, slot_t n) { return math_random_to_dbl(L, args, n, L->random()); }
	static uint64_t math_srandom(vm* L, any* args, slot_t n) { return math_random_to_dbl(L, args, n, platform::srng()); }

	// Unary/Binary functions.
	//
#define REMAP_MATH_UNARY(NAME, ...)																  \
	static double LI_CC LI_STRCAT(__math_, NAME)(double x) { return __VA_ARGS__; }  \
	static const nfunc_info LI_STRCAT(LI_STRCAT(__math_, NAME), _info) = {			  \
		.is_pure   = true,																			  \
		.is_const  = true,																			  \
		.no_throw  = true,																			  \
		.name      = "math." LI_STRINGIFY(NAME),												  \
		.invoke = [](vm* L, any* args, slot_t n) {											  \
			if (n == 0 || !args[0].is_num()) {													  \
			  return L->error("expected number");												  \
			}																								  \
			return L->ok(any(LI_STRCAT(__math_, NAME)(args[0].as_num())));				  \
		},																									  \
		.overloads = {nfunc_overload{																  \
		  .cfunc = li::bit_cast<const void*>(&LI_STRCAT(__math_, NAME)),				  \
		  .ret   = ir::type::f64,																	  \
		  .args  = {ir::type::f64},																  \
		}}																									  \
	};
#define REMAP_MATH_BINARY(NAME, ...)																         \
	static double LI_CC LI_STRCAT(__math_, NAME)(double x, double y) { return __VA_ARGS__; }  \
	static const nfunc_info LI_STRCAT(LI_STRCAT(__math_, NAME), _info) = {							\
		.is_pure   = true,																							\
		.is_const  = true,																							\
		.no_throw  = true,																			            \
		.name      = "math." LI_STRINGIFY(NAME),																\
		.invoke = [](vm* L, any* args, slot_t n) {															\
			if (n <= 1 || !args[0].is_num() || !args[-1].is_num()) {										\
			  return L->error("expected two numbers");														\
			}																												\
			return L->ok(any(LI_STRCAT(__math_, NAME)(args[0].as_num(), args[-1].as_num())));	\
		},																													\
		.overloads = {nfunc_overload{																				\
		  .cfunc = li::bit_cast<const void*>(&LI_STRCAT(__math_, NAME)),								\
		  .ret   = ir::type::f64,																					\
		  .args  = {ir::type::f64, ir::type::f64},															\
		}}																													\
	};
	REMAP_MATH_UNARY(rad, x*(180/std::numbers::pi))
	REMAP_MATH_UNARY(deg, x*(std::numbers::pi/180))
	REMAP_MATH_UNARY(rcp, 1/x)
	REMAP_MATH_UNARY(rsqrt, 1/sqrt(x))
	REMAP_MATH_UNARY(sqrt, sqrt(x))
	REMAP_MATH_UNARY(cbrt, cbrt(x))
	REMAP_MATH_UNARY(abs, abs(x))
	REMAP_MATH_UNARY(sgn, copysign(1, x))
	REMAP_MATH_UNARY(cos, cos(x))
	REMAP_MATH_UNARY(sin, sin(x))
	REMAP_MATH_UNARY(tan, tan(x))
	REMAP_MATH_UNARY(acos, acos(x))
	REMAP_MATH_UNARY(asin, asin(x))
	REMAP_MATH_UNARY(atan, atan(x))
	REMAP_MATH_UNARY(floor, floor(x))
	REMAP_MATH_UNARY(ceil, ceil(x))
	REMAP_MATH_UNARY(trunc, trunc(x))
	REMAP_MATH_UNARY(round, round(x))
	REMAP_MATH_UNARY(log, log(x))
	REMAP_MATH_UNARY(log2, log2(x))
	REMAP_MATH_UNARY(log10, log10(x))
	REMAP_MATH_UNARY(exp, exp(x))
	REMAP_MATH_UNARY(exp2, exp2(x))
	REMAP_MATH_BINARY(min, fmin(x, y))
	REMAP_MATH_BINARY(max, fmax(x, y))
	REMAP_MATH_BINARY(copysign, copysign(x, y))
	REMAP_MATH_BINARY(atan2, atan2(x, y))

	// Registers the math library.
	//
	void detail::register_math(vm* L) {
		// Constants.
		//
		util::export_as(L, "math.FAST", bool(LI_FAST_MATH));
		util::export_as(L, "math.EPS", std::numeric_limits<double>::epsilon());
		util::export_as(L, "math.INF", std::numeric_limits<double>::infinity());
		util::export_as(L, "math.NaN", std::numeric_limits<double>::quiet_NaN());
		util::export_as(L, "math.MAX", std::numeric_limits<double>::max());
		util::export_as(L, "math.MIN", std::numeric_limits<double>::min());
		util::export_as(L, "math.PI", std::numbers::pi);
		util::export_as(L, "math.E",  std::numbers::e);

		// Random.
		//
		util::export_as(L, "math.random", math_random);
		util::export_as(L, "math.srandom", math_srandom);

		// Misc functions.
		//
		util::export_nf(L, &__math_rad_info);
		util::export_nf(L, &__math_deg_info);
		util::export_nf(L, &__math_rcp_info);
		util::export_nf(L, &__math_rsqrt_info);
		util::export_nf(L, &__math_sqrt_info);
		util::export_nf(L, &__math_cbrt_info);
		util::export_nf(L, &__math_abs_info);
		util::export_nf(L, &__math_sgn_info);
		util::export_nf(L, &__math_cos_info);
		util::export_nf(L, &__math_sin_info);
		util::export_nf(L, &__math_tan_info);
		util::export_nf(L, &__math_acos_info);
		util::export_nf(L, &__math_asin_info);
		util::export_nf(L, &__math_atan_info);
		util::export_nf(L, &__math_floor_info);
		util::export_nf(L, &__math_ceil_info);
		util::export_nf(L, &__math_trunc_info);
		util::export_nf(L, &__math_round_info);
		util::export_nf(L, &__math_log_info);
		util::export_nf(L, &__math_log2_info);
		util::export_nf(L, &__math_log10_info);
		util::export_nf(L, &__math_exp_info);
		util::export_nf(L, &__math_exp2_info);
		util::export_nf(L, &__math_min_info);
		util::export_nf(L, &__math_max_info);
		util::export_nf(L, &__math_copysign_info);
		util::export_nf(L, &__math_atan2_info);
	}
};
