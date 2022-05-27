#include <vm/state.hpp>
#include <vm/table.hpp>

namespace li {
	vm* vm::create(fn_alloc alloc, void* allocu, size_t context_space) {
		// Allocate the first page.
		//
		size_t length = (std::max(gc::minimum_allocation, sizeof(vm) + context_space) + 0xFFF) >> 12;
		auto*  ptr    = alloc(allocu, nullptr, length, false);
		if (!ptr)
			return nullptr;

		// Initialize the GC page, create the VM.
		//
		auto* gc    = new (ptr) gc::page(length);
		vm*   L     = gc->create<vm>(nullptr, context_space);
		L->gc.alloc_fn = alloc;
		L->gc.alloc_ctx = allocu;
		util::link_after(&L->gc.page_list_head, gc);
		init_string_intern(L);
		L->globals    = table::create(L, 32);
		L->stack      = (any*) malloc(sizeof(any) * 32);
		L->stack_len  = 32;
		return L;
	}

	// Closes the VM state.
	//
	void vm::close() {
		free(stack);
		gc.close();
	}
};