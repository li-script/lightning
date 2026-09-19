#pragma once
#include <util/common.hpp>
#include <vm/types.hpp>

namespace li {
	struct vm;

	// Logical iterator result. value owns one reference when ok is true, including
	// when done is true; the caller must release it. done is independent of value,
	// so nil is an ordinary yielded value rather than an exhaustion sentinel.
	struct next_result {
		any  value = nil;
		bool done  = true;
		bool ok    = true;
	};

	// Advances an iterator value through its next trait (or the coroutine/internal
	// iterator fast path). The returned value follows next_result ownership.
	next_result iterator_next(vm* L, any_t iterator);

	// Shared ITER implementation for the interpreter, JIT, and native consumers.
	// state/key/value are owning slots initialized by the caller. Returns true for
	// a yielded value, false for exhaustion,
	// and exception_marker for failure. On failure all three slots are unchanged.
	any_t LI_CC iterator_step(vm* L, any_t iterable, any& state, any& key, any& value);

	namespace lib {
		// Installs range.create and iterator.next/map/filter.
		void register_iterator(vm* L);
	}
}
