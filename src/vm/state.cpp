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

		// Initialize a GC state.
		//
		gc::state gc{};
		gc.alloc_fn           = alloc;
		gc.alloc_ctx          = allocu;
		gc.initial_page       = new (ptr) gc::page(length, false);
		gc.initial_page->num_indeps = 1; // VM itself is never enumerated.

		// Create the VM.
		//
		vm* L = gc.create<vm>(nullptr, context_space);
		L->gc = gc;

		// Initialize stack.
		//
		L->stack    = (any*) alloc(allocu, 0, (STACK_LENGTH * sizeof(any)) >> 12, false);
		L->stack_top = L->stack;
		if (!L->stack) [[unlikely]] {
			alloc(allocu, ptr, length, false);
			return nullptr;
		}

		// Initialize globals and string interning state.
		//
		L->globals = table::create(L, vm::reserved_global_length);
		strset_init(L);
		return L;
	}

	// Closes the VM state.
	//
	void vm::close()
	{
		gc.alloc_fn(gc.alloc_ctx, stack, (STACK_LENGTH * sizeof(any)) >> 12, false);
		gc.close(this);
	}
};