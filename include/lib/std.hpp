#pragma once
#include <util/common.hpp>
#include <vm/state.hpp>
#include <vm/function.hpp>
#include <util/user.hpp>

namespace li::lib {
	// Registers the builtins, these are called upon VM creation as they are required.
	//
	namespace detail {
		extern util::native_function math_rad;
		extern util::native_function math_deg;
		extern util::native_function math_sqrt;
		extern util::native_function math_cbrt;
		extern util::native_function math_abs;
		extern util::native_function math_sgn;
		extern util::native_function math_cos;
		extern util::native_function math_sin;
		extern util::native_function math_tan;
		extern util::native_function math_acos;
		extern util::native_function math_asin;
		extern util::native_function math_atan;
		extern util::native_function math_floor;
		extern util::native_function math_ceil;
		extern util::native_function math_trunc;
		extern util::native_function math_round;
		extern util::native_function math_log;
		extern util::native_function math_log2;
		extern util::native_function math_log10;
		extern util::native_function math_exp;
		extern util::native_function math_exp2;
		extern util::native_function math_min;
		extern util::native_function math_max;
		extern util::native_function math_copysign;
		extern util::native_function math_atan2;
		extern util::native_function math_pow;
		extern util::native_function math_mod;

		extern util::native_function builtin_array_new;
		extern util::native_function builtin_table_new;
		extern util::native_function builtin_new_table;
		extern util::native_function builtin_new_array;
		extern util::native_function builtin_len;   // Overloads: [Arr, tbl, str, ?]
		extern util::native_function builtin_join;  // Overloads: [Arr, tbl, str, ?]
		extern util::native_function builtin_in;  // Overloads: [Arr, tbl, str-num, str-str ?]
		extern util::native_function builtin_dup;   // Overloads: [Arr, tbl, fn, obj, ?]
		extern util::native_function builtin_str;
		extern util::native_function builtin_num;
		extern util::native_function builtin_int;
		extern util::native_function builtin_push;
		extern util::native_function builtin_pop;
		extern util::native_function builtin_null_function;

		void register_builtin(vm* L);
		void register_math(vm* L);
	};

	// Registers the chrono library.
	//
	void register_chrono(vm* L);

	// Registers the debug library.
	//
	void register_debug(vm* L);

#if LI_JIT
	// Turns jit on or off.
	//
	void jit_on(vm* L, function* f, bool verbose);
	void jit_off(vm* L, function* f);

	// Registers the JIT library.
	//
	void register_jit(vm* L);
#endif

	// Registers the entire standard library.
	//
	static void register_std(vm* L) {
		register_debug(L);
		register_chrono(L);
#if LI_JIT
		register_jit(L);
#endif
	}
};