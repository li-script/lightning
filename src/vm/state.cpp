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
		auto* gc           = new (ptr) gc::page(length);
		vm*   L            = gc->create<vm>(nullptr, context_space);
		gc->next           = gc;
		gc->prev           = gc;
		L->gc.alloc_fn     = alloc;
		L->gc.alloc_ctx    = allocu;
		L->gc.initial_page = gc;

		// Initialize stack.
		//
		auto* stack  = L->alloc<vm_stack>(vm::initial_stack_length * sizeof(any));
		L->stack_len = uint32_t(stack->extra_bytes() / sizeof(any));
		L->stack     = stack->list;

		// Initialize globals and string interning state.
		//
		L->globals = table::create(L, vm::reserved_global_length);
		init_string_intern(L);
		return L;
	}

	// Closes the VM state.
	//
	void vm::close() { gc.close(); }
};