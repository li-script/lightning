#pragma once
#include <stdint.h>

namespace lightning::core {
	struct vm;

	// Page allocator:
	// - f(nullptr, N, ?) =     allocation
	// - f(ptr,     N, false) = free
	using fn_alloc = void* (*) (vm* L, void* pointer, size_t page_count, bool executable);
};

// Platform-dependant functions.
//
namespace lightning::platform {
	// Invoked to ensure ANSI escapes work.
	//
	void setup_ansi_escapes();

	// Default page allocator.
	//
	void* page_alloc(core::vm* L, void* pointer, size_t page_count, bool executable);
};