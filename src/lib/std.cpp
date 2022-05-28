#include <lib/std.hpp>
#include <util/user.hpp>
#include <vm/function.hpp>
#include <lang/parser.hpp>
#include <cmath>
#include <bit>

namespace li::lib {
#define REMAP_MATH_UNARY(name)																					\
		util::export_as(L, "math." LI_STRINGIFY(name), [](vm* L, const any* args, int32_t n) { \
			if (n != 1 || !args->is(type_number)) {															\
				L->push_stack(any(string::create(L, "expected number")));								\
				return false;																							\
			}																												\
			L->push_stack(any(name(args->as_num())));															\
			return true;																								\
		});

#define REMAP_MATH_BINARY(name)																					\
	util::export_as(L, "math." LI_STRINGIFY(name), [](vm* L, const any* args, int32_t n) {		\
		if (n != 2 || !args[0].is(type_number) || !args[-1].is(type_number)) {						\
			L->push_stack(any(string::create(L, "expected two numbers")));								\
			return false;																								\
		}																													\
		L->push_stack(any(name(args[0].as_num(), args[-1].as_num())));									\
		return true;																									\
	});

	static constexpr double pi = 3.14159265358979323846264338327950288;

	static double rad(double x) { return x * (180 / pi); }
	static double deg(double x) { return x * (pi / 180); }


	static bool math_random_to_dbl(vm* L, const any* args, int32_t n, uint64_t v) {
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
		double r =  __builtin_bit_cast( double, v ) + 0.5;

		// If no args given, return.
		//
		if (n == 0) {
			L->push_stack(r);
			return true;
		}

		// Validate args.
		//
		if (n != 1 && n != 2) {
			return L->error("expected one or two numbers.");
		}

		// If one arg given, generate [0, x] inclusive.
		//
		if (!args[1-n].is(type_number))
			return L->error("expected one or two numbers.");
		double x = args[1-n].as_num();

		// If two args given, generate [y, x] inclusive.
		//
		double y = 0;
		if (n == 2) {
			if (!args[0].is(type_number))
				return L->error("expected one or two numbers.");
			y = args[0].as_num();
		}

		L->push_stack(y + r * (x - y));
		return true;
	}

	static bool math_random(vm* L, const any* args, int32_t n) { return math_random_to_dbl(L, args, n, L->random()); }
	static bool math_srandom(vm* L, const any* args, int32_t n) { return math_random_to_dbl(L, args, n, platform::srng()); }

	// Registers standard library.
	//
	void register_std(vm* L) {
		// Math library.
		//
		util::export_as(L, "math.random", math_random);
		util::export_as(L, "math.srandom", math_srandom);
		util::export_as(L, "math.eps", std::numeric_limits<double>::epsilon());
		util::export_as(L, "math.inf", std::numeric_limits<double>::infinity());
		util::export_as(L, "math.nan", std::numeric_limits<double>::quiet_NaN());
		util::export_as(L, "math.pi", pi);

		static constexpr auto max = [](double a, double b) { return fmax(a, b); };
		static constexpr auto min = [](double a, double b) { return fmin(a, b); };
		static constexpr auto abs = [](double a) { return fabs(a); };

		REMAP_MATH_BINARY(min);
		REMAP_MATH_BINARY(max);
		REMAP_MATH_BINARY(atan2);
		REMAP_MATH_UNARY(deg);
		REMAP_MATH_UNARY(rad);
		REMAP_MATH_UNARY(sqrt);
		REMAP_MATH_UNARY(abs);
		REMAP_MATH_UNARY(cos);
		REMAP_MATH_UNARY(sin);
		REMAP_MATH_UNARY(tan);
		REMAP_MATH_UNARY(round);
		REMAP_MATH_UNARY(ceil);
		REMAP_MATH_UNARY(floor);
		REMAP_MATH_UNARY(acos);
		REMAP_MATH_UNARY(asin);
		REMAP_MATH_UNARY(atan);
		REMAP_MATH_UNARY(log);
		REMAP_MATH_UNARY(log10);

		// String.
		//
		util::export_as(L, "print", [](vm* L, const any* args, int32_t n) {
			for (size_t i = 0; i != n; i++) {
				args[-i].print();
				printf("\t");
			}
			printf("\n");
			return true;
		});
		util::export_as(L, "tostring", [](vm* L, const any* args, int32_t n) {
			if (n == 0) {
				L->push_stack(string::create(L));
				return true;
			} else {
				L->push_stack(args->coerce_str(L));
				return true;
			}
		});
		util::export_as(L, "tonumber", [](vm* L, const any* args, int32_t n) {
			if (n == 0) {
				L->push_stack(any(number(0)));
				return true;
			} else {
				L->push_stack(args->coerce_num());
				return true;
			}
		});
		util::export_as(L, "loadstring", [](vm* L, const any* args, int32_t n) {
			if (n != 1 || !args->is(type_string)) {
				return L->error("expected string");
			}
			auto res = load_script(L, args->as_str()->view());
			L->push_stack(res);
			return res.is(type_function);
		});
		util::export_as(L, "eval", [](vm* L, const any* args, int32_t n) {
			if (n != 1 || !args->is(type_string)) {
				return L->error("expected string");
			}
			auto res = load_script(L, args->as_str()->view());
			if (!res.is(type_function)) {
				return false;
			}
			return L->scall(0, res);
		});

		// Misc.
		//
		util::export_as(L, "assert", [](vm* L, const any* args, int32_t n) {
			if (!n || args->as_bool())
				return true;

			if (n >= 2 && args[-1].is(type_string)) {
				L->push_stack(args[-1]);
				return false;
			} else {
				// TODO: Use debug info to provide line information.
				return L->error("assertion failed.");
			}
		});
		util::export_as(L, "@gc", [](vm* L, const any* args, int32_t n) {
			L->gc.collect(L);
			return true;
		});
		util::export_as(L, "@printbc", [](vm* L, const any* args, int32_t n) {
			if (n != 1 || !args->is(type_function)) {
				return L->error("@printbc expects a single vfunction");
			}

			auto f = args->as_vfn();
			puts(
				 "Dumping bytecode of the function:\n"
				 "-----------------------------------------------------");
			for (uint32_t i = 0; i != f->length; i++) {
				f->opcode_array[i].print(i);
			}
			puts("-----------------------------------------------------");
			for (uint32_t i = 0; i != f->num_uval; i++) {
				printf(LI_CYN "u%u:   " LI_DEF, i);
				f->uvals()[i].print();
				printf("\n");
			}
			puts("-----------------------------------------------------");
			return true;
		});
	}
};
