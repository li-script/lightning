#pragma once
#include <util/common.hpp>
#include <vm/state.hpp>
#include <vm/function.hpp>

namespace li::lib {
	// Registers the builtins, these are called upon VM creation as they are required.
	//
	namespace detail {
		extern nfunc_info math_rad_info;
		extern nfunc_info math_deg_info;
		extern nfunc_info math_sqrt_info;
		extern nfunc_info math_cbrt_info;
		extern nfunc_info math_abs_info;
		extern nfunc_info math_sgn_info;
		extern nfunc_info math_cos_info;
		extern nfunc_info math_sin_info;
		extern nfunc_info math_tan_info;
		extern nfunc_info math_acos_info;
		extern nfunc_info math_asin_info;
		extern nfunc_info math_atan_info;
		extern nfunc_info math_floor_info;
		extern nfunc_info math_ceil_info;
		extern nfunc_info math_trunc_info;
		extern nfunc_info math_round_info;
		extern nfunc_info math_log_info;
		extern nfunc_info math_log2_info;
		extern nfunc_info math_log10_info;
		extern nfunc_info math_exp_info;
		extern nfunc_info math_exp2_info;
		extern nfunc_info math_min_info;
		extern nfunc_info math_max_info;
		extern nfunc_info math_copysign_info;
		extern nfunc_info math_atan2_info;
		extern nfunc_info math_pow_info;
		extern nfunc_info math_mod_info;

		extern nfunc_info builtin_array_new_info;
		extern nfunc_info builtin_table_new_info;
		extern nfunc_info builtin_dup_table_info;
		extern nfunc_info builtin_dup_array_info;
		extern nfunc_info builtin_dup_function_info;
		extern nfunc_info builtin_new_table_info;
		extern nfunc_info builtin_new_array_info;

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