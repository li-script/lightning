#include <vm/state.hpp>
#include <vm/table.hpp>
#include <lib/std.hpp>

namespace li {
	vm* vm::create(fn_alloc alloc, void* allocu) {
		// Allocate the first page.
		//
		size_t length = (std::max(gc::minimum_allocation, sizeof(vm) + STACK_LENGTH * sizeof(any)) + 0xFFF) >> 12;
		auto*  ptr    = alloc(allocu, nullptr, length, false);
		if (!ptr)
			return nullptr;

		// Initialize a GC state.
		//
		gc::state gc{};
		gc.alloc_fn                   = alloc;
		gc.alloc_ctx                  = allocu;
		gc.initial_page               = new (ptr) gc::page(length, false);
		gc.initial_page->is_permanent = 1;  // Cannot delete initial page.

		// Create the VM.
		//
		vm* L        = gc.create<vm>(nullptr, STACK_LENGTH * sizeof(any));
		L->gc        = gc;
		L->stack_top = L->stack;

		// Initialize globals and string interning state.
		//
		L->modules = table::create(L, 32);
		L->modules->set_trait(L, trait::freeze, true);
		L->modules->set_trait(L, trait::seal, true);
		strset_init(L);
		lib::detail::register_builtin(L);
		lib::detail::register_math(L);
		return L;
	}

	// Closes the VM state.
	//
	void vm::close() { gc.close(this); }
};