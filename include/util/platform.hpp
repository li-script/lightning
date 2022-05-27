#pragma once
#include <stdint.h>

namespace li {
	struct vm;

	// Page allocator:
	// - f(ctx, nullptr, N, ?) =     allocation
	// - f(ctx, ptr,     N, false) = free
	// - f(ctx, ctx,     0, false) = close state
	//
	using fn_alloc = void* (*) (void* ud, void* pointer, size_t page_count, bool executable);
};

// Platform-dependant functions.
//
namespace li::platform {
	// Invoked to ensure ANSI escapes work.
	//
	void setup_ansi_escapes();

	// Default page allocator.
	//
	void* page_alloc(void* ud, void* pointer, size_t page_count, bool executable);
};