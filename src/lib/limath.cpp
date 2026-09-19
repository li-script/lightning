#include <algorithm>
#include <bit>
#include <cmath>
#include <lang/parser.hpp>
#include <lib/std.hpp>
#include <numbers>
#include <numeric>
#include <util/user.hpp>

namespace li::lib {
	static any_t math_random_to_dbl(vm* L, any* args, slot_t n, uint64_t v) {
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
		if (!args[1 - n].is_num() || !std::isfinite(args[1 - n].as_num()))
			return L->error("expected one or two finite numbers");
		double x = args[1 - n].as_num();

		// If two args given, generate [y, x] inclusive.
		//
		double y = 0;
		if (n == 2) {
			if (!args[0].is_num() || !std::isfinite(args[0].as_num()))
				return L->error("expected one or two finite numbers");
			y = args[0].as_num();
		}
		return L->ok(y + r * (x - y));
	}
	static any_t math_random(vm* L, any* args, slot_t n) { return math_random_to_dbl(L, args, n, L->random()); }
	static any_t math_srandom(vm* L, any* args, slot_t n) { return math_random_to_dbl(L, args, n, platform::srng()); }

	// Unary/Binary functions.
	//
#define REMAP_MATH_UNARY(NAME, INTRINSIC, ...)                                                                 \
	static double LI_CC   LI_STRCAT(c_math_, NAME)(double x) { return __VA_ARGS__; }                            \
	util::native_function detail::LI_STRCAT(math_, NAME) = {                                                    \
		 func_attr_pure | func_attr_const,                                                                       \
		 "math." LI_STRINGIFY(NAME),                                                                             \
		 [](vm* L, any* args, slot_t n) {                                                                        \
			 if (n != 1 || !args[0].is_num()) {                                                                   \
				 return L->error("expected one number");                                                           \
			 }                                                                                                    \
			 return L->ok(any(LI_STRCAT(c_math_, NAME)(args[0].as_num())));                                       \
		 },                                                                                                      \
		 {{li::bit_cast<const void*>(&LI_STRCAT(c_math_, NAME)), {type::f64}, type::f64, intrinsic::INTRINSIC}}, \
	};
#define REMAP_MATH_BINARY(NAME, INTRINSIC, ...)                                                                           \
	static double LI_CC   LI_STRCAT(c_math_, NAME)(double x, double y) { return __VA_ARGS__; }                             \
	util::native_function detail::LI_STRCAT(math_, NAME) = {                                                               \
		 func_attr_pure | func_attr_const,                                                                                  \
		 "math." LI_STRINGIFY(NAME),                                                                                        \
		 [](vm* L, any* args, slot_t n) {                                                                                   \
			 if (n != 2 || !args[0].is_num() || !args[-1].is_num()) {                                                        \
				 return L->error("expected two numbers");                                                                     \
			 }                                                                                                               \
			 return L->ok(any(LI_STRCAT(c_math_, NAME)(args[0].as_num(), args[-1].as_num())));                               \
		 },                                                                                                                 \
		 {{li::bit_cast<const void*>(&LI_STRCAT(c_math_, NAME)), {type::f64, type::f64}, type::f64, intrinsic::INTRINSIC}}, \
	};

	REMAP_MATH_UNARY(rad, none, x*(std::numbers::pi / 180))
	REMAP_MATH_UNARY(deg, none, x * (180 / std::numbers::pi))
	REMAP_MATH_UNARY(sqrt, sqrt, sqrt(x))
	REMAP_MATH_UNARY(cbrt, none, cbrt(x))
	REMAP_MATH_UNARY(abs, abs, std::abs(x))
	REMAP_MATH_UNARY(cos, none, cos(x))
	REMAP_MATH_UNARY(sin, none, sin(x))
	REMAP_MATH_UNARY(tan, none, tan(x))
	REMAP_MATH_UNARY(acos, none, acos(x))
	REMAP_MATH_UNARY(asin, none, asin(x))
	REMAP_MATH_UNARY(atan, none, atan(x))
	REMAP_MATH_UNARY(floor, floor, floor(x))
	REMAP_MATH_UNARY(ceil, ceil, ceil(x))
	REMAP_MATH_UNARY(trunc, trunc, trunc(x))
	REMAP_MATH_UNARY(round, round, round(x))
	REMAP_MATH_UNARY(log, none, log(x))
	REMAP_MATH_UNARY(log2, none, log2(x))
	REMAP_MATH_UNARY(log10, none, log10(x))
	REMAP_MATH_UNARY(exp, none, exp(x))
	REMAP_MATH_UNARY(exp2, none, exp2(x))
	REMAP_MATH_BINARY(min, min, fmin(x, y))
	REMAP_MATH_BINARY(max, max, fmax(x, y))
	REMAP_MATH_BINARY(copysign, copysign, copysign(x, y))
	REMAP_MATH_BINARY(atan2, none, atan2(x, y))
	REMAP_MATH_BINARY(pow, none, pow(x, y))
	REMAP_MATH_BINARY(mod, none, fmod(x, y))

	static bool math_arguments(vm* L, any* args, slot_t count, slot_t expected, const char* operation) {
		if (count != expected) {
			L->error("math.%s expects %d number%s", operation, expected, expected == 1 ? "" : "s");
			return false;
		}
		for (slot_t index = 0; index != expected; ++index) {
			if (!args[-index].is_num()) {
				L->error("math.%s expects %d number%s", operation, expected, expected == 1 ? "" : "s");
				return false;
			}
		}
		return true;
	}

	static any_t math_clamp(vm* L, any* args, slot_t count) {
		if (!math_arguments(L, args, count, 3, "clamp"))
			return exception_marker;
		const number value = args[0].as_num();
		const number lower = args[-1].as_num();
		const number upper = args[-2].as_num();
		if (std::isnan(lower) || std::isnan(upper) || lower > upper)
			return L->error("math.clamp requires ordered non-NaN bounds");
		return L->ok(std::clamp(value, lower, upper));
	}

	static any_t math_lerp(vm* L, any* args, slot_t count) {
		if (!math_arguments(L, args, count, 3, "lerp"))
			return exception_marker;
		return L->ok(std::lerp(args[0].as_num(), args[-1].as_num(), args[-2].as_num()));
	}

	static any_t math_sign(vm* L, any* args, slot_t count) {
		if (!math_arguments(L, args, count, 1, "sign"))
			return exception_marker;
		const number value = args[0].as_num();
		return L->ok(value > 0 ? number(1) : value < 0 ? number(-1) : value);
	}

	static any_t math_isnan(vm* L, any* args, slot_t count) {
		if (!math_arguments(L, args, count, 1, "isnan"))
			return exception_marker;
		return L->ok(std::isnan(args[0].as_num()));
	}

	static any_t math_isfinite(vm* L, any* args, slot_t count) {
		if (!math_arguments(L, args, count, 1, "isfinite"))
			return exception_marker;
		return L->ok(std::isfinite(args[0].as_num()));
	}

	static any_t math_isinteger(vm* L, any* args, slot_t count) {
		if (!math_arguments(L, args, count, 1, "isinteger"))
			return exception_marker;
		const number value = args[0].as_num();
		return L->ok(std::isfinite(value) && value == std::trunc(value));
	}

	static constexpr number maximum_safe_integer = 9007199254740991.0;

	static bool checked_math_integer(vm* L, any_t value, const char* operation, int64_t& result) {
		if (!value.is_num() || !std::isfinite(value.as_num()) || value.as_num() != std::trunc(value.as_num()) ||
			 std::abs(value.as_num()) > maximum_safe_integer) {
			L->error("math.%s expects safe integers", operation);
			return false;
		}
		result = static_cast<int64_t>(value.as_num());
		return true;
	}

	static uint64_t integer_magnitude(int64_t value) { return value < 0 ? uint64_t(-value) : uint64_t(value); }

	static any_t math_gcd(vm* L, any* args, slot_t count) {
		if (count != 2)
			return L->error("math.gcd expects two safe integers");
		int64_t lhs = 0;
		int64_t rhs = 0;
		if (!checked_math_integer(L, args[0], "gcd", lhs) || !checked_math_integer(L, args[-1], "gcd", rhs))
			return exception_marker;
		return L->ok(number(std::gcd(lhs, rhs)));
	}

	static any_t math_lcm(vm* L, any* args, slot_t count) {
		if (count != 2)
			return L->error("math.lcm expects two safe integers");
		int64_t lhs = 0;
		int64_t rhs = 0;
		if (!checked_math_integer(L, args[0], "lcm", lhs) || !checked_math_integer(L, args[-1], "lcm", rhs))
			return exception_marker;
		if (lhs == 0 || rhs == 0)
			return L->ok(number(0));
		const uint64_t divisor = uint64_t(std::gcd(lhs, rhs));
		const uint64_t left    = integer_magnitude(lhs) / divisor;
		const uint64_t right   = integer_magnitude(rhs);
		if (left > uint64_t(maximum_safe_integer) / right)
			return L->error("math.lcm result exceeds the safe integer range");
		return L->ok(number(left * right));
	}

	// Registers the math library.
	//
	void detail::register_math(vm* L) {
		// Constants.
		//
		util::export_as(L, "math.fast", bool(LI_FAST_MATH));
		util::export_as(L, "math.epsilon", std::numeric_limits<double>::epsilon());
		util::export_as(L, "math.inf", std::numeric_limits<double>::infinity());
		util::export_as(L, "math.nan", std::numeric_limits<double>::quiet_NaN());
		util::export_as(L, "math.huge", std::numeric_limits<double>::max());
		util::export_as(L, "math.small", std::numeric_limits<double>::min());
		util::export_as(L, "math.pi", std::numbers::pi);
		util::export_as(L, "math.e", std::numbers::e);

		// Random.
		//
		util::export_as(L, "math.random", math_random);
		util::export_as(L, "math.srandom", math_srandom);

		// Misc functions.
		//
		math_rad.export_into(L);
		math_deg.export_into(L);
		math_sqrt.export_into(L);
		math_cbrt.export_into(L);
		math_abs.export_into(L);
		math_cos.export_into(L);
		math_sin.export_into(L);
		math_tan.export_into(L);
		math_acos.export_into(L);
		math_asin.export_into(L);
		math_atan.export_into(L);
		math_floor.export_into(L);
		math_ceil.export_into(L);
		math_trunc.export_into(L);
		math_round.export_into(L);
		math_log.export_into(L);
		math_log2.export_into(L);
		math_log10.export_into(L);
		math_exp.export_into(L);
		math_exp2.export_into(L);
		math_min.export_into(L);
		math_max.export_into(L);
		math_copysign.export_into(L);
		math_atan2.export_into(L);
		math_pow.export_into(L);
		math_mod.export_into(L);
		util::export_as(L, "math.clamp", math_clamp);
		util::export_as(L, "math.lerp", math_lerp);
		util::export_as(L, "math.sign", math_sign);
		util::export_as(L, "math.isnan", math_isnan);
		util::export_as(L, "math.isfinite", math_isfinite);
		util::export_as(L, "math.isinteger", math_isinteger);
		util::export_as(L, "math.gcd", math_gcd);
		util::export_as(L, "math.lcm", math_lcm);

		// TODO: BC lifter for math_rad/math_deg?
	}
};
