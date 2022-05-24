#include <vm/state.hpp>
#include <vm/table.hpp>

namespace lightning::core {
	// Closes the VM state.
	//
	void vm::close() {
		auto* alloc = alloc_fn;
		auto* head =  &gc_page_head;
		auto  hcopy = *head;

		free(stack);
		for (auto it = gc_page_head.prev; it != &gc_page_head; it = it->prev) {
			alloc(this, it, it->num_pages, false);
		}
	}

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
		L->globals    = table::create(L, 32);
		L->stack      = (any*) malloc(sizeof(any) * 32);
		L->stack_len  = 32;
		memset(L->stack, 0xFF, sizeof(any) * 32);
		return L;
	}
};