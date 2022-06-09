#pragma once
#include <util/common.hpp>
#include <vm/state.hpp>

namespace li::lib {
	// Registers the builtins, this is called by the VM creation as it is required.
	//
	namespace detail {
		void register_builtin(vm* L);
	};

	// Registers the chrono library.
	//
	void register_chrono(vm* L);

	// Registers the debug library.
	//
	void register_debug(vm* L);

	// Registers the math library.
	//
	void register_math(vm* L);

#if LI_JIT
	// Registers the JIT library.
	//
	void register_jit(vm* L);
#endif

	// Registers the entire standard library.
	//
	static void register_std(vm* L) {
		register_debug(L);
		register_math(L);
		register_chrono(L);
#if LI_JIT
		register_jit(L);
#endif
	}
};