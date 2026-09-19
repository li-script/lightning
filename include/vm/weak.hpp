#pragma once
#include <vm/gc.hpp>

namespace li {
	struct weak;

	namespace gc {
		// Invalidates every weak observer before a target begins destruction.
		void invalidate_weak(vm* L, header* target);

		// Unregisters a weak object during destruction.
		void destroy(vm* L, weak* value);

		// Invalidates and releases all weak-registry storage during VM shutdown.
		void shutdown_weak(vm* L);
	}

	struct weak : gc::leaf<weak, type_weak> {
		// Creates a non-owning observer of target.
		static weak* create(vm* L, gc::header* target);

		// Returns a new owned strong reference to the target, or nil when expired.
		any_t lock() const;

		// Reports whether the observed target has been destroyed.
		bool expired() const;

		weak() = default;

	  private:
		gc::header* target          = nullptr;
		bool        observes_shared = false;

		friend void gc::invalidate_weak(vm* L, gc::header* target);
		friend void gc::destroy(vm* L, weak* value);
		friend void gc::shutdown_weak(vm* L);
	};
}
