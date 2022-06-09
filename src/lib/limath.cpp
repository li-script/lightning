#include <lib/std.hpp>
#include <util/user.hpp>
#include <lang/parser.hpp>
#include <cmath>
#include <bit>
#include <numbers>

namespace li::lib {
#define REMAP_MATH_UNARY(name)                                                     \
	util::export_as(L, "math." LI_STRINGIFY(name), [](vm* L, any* args, slot_t n) { \
		if (n != 1 || !args->is_num()) {                                             \
			return L->error("expected number");                 \
		}                                                                            \
		return L->ok(any(name(args->as_num())));                                    \
	});

#define REMAP_MATH_BINARY(name)                                                    \
	util::export_as(L, "math." LI_STRINGIFY(name), [](vm* L, any* args, slot_t n) { \
		if (n != 2 || !args[0].is_num() || !args[-1].is_num()) {                     \
			return L->error("expected two numbers");            \
		}                                                                            \
		return L->ok(any(name(args[0].as_num(), args[-1].as_num())));               \
	});

	static double rad(double x) { return x * (180 / std::numbers::pi); }
	static double deg(double x) { return x * (std::numbers::pi / 180); }

	static bool math_random_to_dbl(vm* L, any* args, slot_t n, uint64_t v) {
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
		v = std::rotl( v, mantissa_bits );
		double r = bit_cast<double>(v) + 0.5;

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
		if (!args[1-n].is_num())
			return L->error("expected one or two numbers.");
		double x = args[1-n].as_num();

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

	static bool math_random(vm* L, any* args, slot_t n) { return math_random_to_dbl(L, args, n, L->random()); }
	static bool math_srandom(vm* L, any* args, slot_t n) { return math_random_to_dbl(L, args, n, platform::srng()); }

	// Registers the math library.
	//
	void register_math(vm* L) {
		// Math library.
		//
		util::export_as(L, "math.fast", bool(LI_FAST_MATH));
		util::export_as(L, "math.random", math_random);
		util::export_as(L, "math.srandom", math_srandom);
		util::export_as(L, "math.eps", std::numeric_limits<double>::epsilon());
		util::export_as(L, "math.inf", std::numeric_limits<double>::infinity());
		util::export_as(L, "math.nan", std::numeric_limits<double>::quiet_NaN());
		util::export_as(L, "math.pi", std::numbers::pi);
		util::export_as(L, "math.e",  std::numbers::e);

		static constexpr auto max = [](double a, double b) { return fmax(a, b); };
		static constexpr auto min = [](double a, double b) { return fmin(a, b); };
		static constexpr auto abs = [](double a) { return fabs(a); };
		static constexpr auto sgn = [](double a) -> double { return a < 0 ? -1 : +1; };

		REMAP_MATH_BINARY(min);
		REMAP_MATH_BINARY(copysign);
		REMAP_MATH_BINARY(max);
		REMAP_MATH_BINARY(atan2);
		REMAP_MATH_UNARY(deg);
		REMAP_MATH_UNARY(rad);
		REMAP_MATH_UNARY(sqrt);
		REMAP_MATH_UNARY(abs);
		REMAP_MATH_UNARY(cos);
		REMAP_MATH_UNARY(sin);
		REMAP_MATH_UNARY(tan);
		REMAP_MATH_UNARY(sgn);
		REMAP_MATH_UNARY(round);
		REMAP_MATH_UNARY(ceil);
		REMAP_MATH_UNARY(floor);
		REMAP_MATH_UNARY(acos);
		REMAP_MATH_UNARY(asin);
		REMAP_MATH_UNARY(atan);
		REMAP_MATH_UNARY(log);
		REMAP_MATH_UNARY(log10);
	}
};
