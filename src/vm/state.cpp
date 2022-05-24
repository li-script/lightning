#include <vm/state.hpp>

namespace lightning::core {
	vm* vm::create(fn_alloc alloc, size_t context_space) {
		// Allocate the first page.
		//
		size_t length = (std::max(minimum_gc_allocation, sizeof(vm) + context_space) + 0xFFF) >> 12;
		auto*  ptr    = alloc(nullptr, nullptr, length, false);
		if (!ptr)
			return nullptr;

		// Initialize the GC page, create the VM.
		//
		auto* gc    = new (ptr) gc_page(length);
		vm*   L     = gc->create<vm>(context_space);
		L->alloc_fn = alloc;
		util::link_after(&L->gc_page_head, gc);
		L->str_intern = create_string_set(L);
		return L;
	}
};