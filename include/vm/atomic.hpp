#pragma once
#include <vm/state.hpp>

namespace li {
	struct function_proto;

	namespace util {
		struct native_function;
	}

	namespace atomic {
		// Returns a diagnostic for bytecode that cannot be executed by the
		// restricted transaction interpreter. A null result means the prototype is
		// structurally valid. Captured values are checked before the first lock;
		// every accessed field is checked before the transaction publishes a write.
		const char* validate_plan(function_proto* prototype) noexcept;

		namespace detail {
			// Private parser helpers. These are static native function objects so
			// parser-emitted calls do not depend on a user-rebindable module export.
			extern util::native_function make_shared;
			extern util::native_function lock_shared;
			extern util::native_function unlock_shared;
			extern util::native_function execute;
			extern util::native_function capture;
		}
	}
}
