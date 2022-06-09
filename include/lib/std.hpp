#pragma once
#include <util/common.hpp>
#include <vm/state.hpp>

namespace li::lib {
	// Registers the builtins, these are called upon VM creation as they are required.
	//
	namespace detail {
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