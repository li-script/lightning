#include <lib/std.hpp>
#include <util/user.hpp>
#include <vm/function.hpp>
#include <cmath>

namespace li::lib {
#define REMAP_MATH_UNARY(name)																					\
		util::export_as(L, "math." LI_STRINGIFY(name), [](vm* L, const any* args, uint32_t n) {\
			if (n != 1 || !args->is(type_number)) {															\
				L->push_stack(any(string::create(L, "expected number")));								\
				return false;																							\
			}																												\
			L->push_stack(any(name(args->as_num())));															\
			return true;																								\
		});

#define REMAP_MATH_BINARY(name)																					\
	util::export_as(L, "math." LI_STRINGIFY(name), [](vm* L, const any* args, uint32_t n) {	\
		if (n != 2 || !args[0].is(type_number) || !args[2].is(type_number)) {						\
			L->push_stack(any(string::create(L, "expected two numbers")));								\
			return false;																								\
		}																													\
		L->push_stack(any(name(args[0].as_num(), args[1].as_num())));									\
		return true;																									\
	});

	static constexpr double pi = 3.14159265358979323846264338327950288;

	static double rad(double x) { return x * (180 / pi); }
	static double deg(double x) { return x * (pi / 180); }

	// Registers standard library.
	//
	void register_std(vm* L) {
		// Math library.
		//
		util::export_as(L, "math.eps", std::numeric_limits<double>::epsilon());
		util::export_as(L, "math.inf", std::numeric_limits<double>::infinity());
		util::export_as(L, "math.nan", std::numeric_limits<double>::quiet_NaN());
		util::export_as(L, "math.pi",  pi);

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
		util::export_as(L, "print", [](vm* L, const any* args, uint32_t n) {
			for (size_t i = 0; i != n; i++) {
				args[i].print();
				printf("\t");
			}
			printf("\n");
			return true;
		});
		util::export_as(L, "tostring", [](vm* L, const any* args, uint32_t n) {
			if (n == 0) {
				L->push_stack(string::create(L));
				return true;
			} else {
				L->push_stack(args->coerce_str(L));
				return true;
			}
		});
		util::export_as(L, "tonumber", [](vm* L, const any* args, uint32_t n) {
			if (n == 0) {
				L->push_stack(any(number(0)));
				return true;
			} else {
				L->push_stack(args->coerce_num());
				return true;
			}
		});

		// Misc.
		//
		util::export_as(L, "@assert", [](vm* L, const any* args, uint32_t n) {
			if (!n || args->as_bool())
				return true;

			if (n >= 2 && args[1].is(type_string)) {
				L->push_stack(args[1]);
				return false;
			} else {
				L->push_stack(string::create(L, "assertion failed."));
				return false;
			}
		});
		util::export_as(L, "@gc", [](vm* L, const any* args, uint32_t n) {
			L->gc.collect(L);
			return true;
		});
		util::export_as(L, "@printbc", [](vm* L, const any* args, uint32_t n) {
			if (n != 1 || !args->is(type_function)) {
				L->push_stack(string::create(L, "@printbc expects a single vfunction"));
				return false;
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
