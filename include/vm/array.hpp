#pragma once
#include <bit>
#include <span>
#include <vm/state.hpp>

namespace lightning::core {
	struct array_store : gc_leaf<array_store> {
		any entries[];
	};
	struct array : gc_node<array> {
		static array* create(vm* L, size_t reserved_entry_count = 0);

		array_store* storage = nullptr;
		size_t       length  = 0;
		any*         begin() { return storage ? storage->entries : nullptr; }
		any*         end() { return begin() + size(); }
		size_t       size() const { return length; }
		size_t       capacity() const { return storage ? storage->object_bytes() / sizeof(any) : 0; }

		// Duplicates the array.
		//
		array* duplicate(vm* L);

		// GC enumerator.
		//
		template<typename F>
		void enum_for_gc(F&& fn) {
			for (auto& e : *this) {
				if (e.is_gc())
					fn(e.as_gc());
			}
		}

		// Reserve and resize.
		//
		void reserve(vm* L, size_t n);
		void resize(vm* L, size_t n);

		// Push-back.
		//
		void push(vm* L, any value);

		// Pop-back.
		//
		any pop();

		// Get/set.
		// - Set returns false if it should throw because of out-of-boundaries index.
		//
		bool set(vm* L, size_t idx, any value);
		any get(vm* L, size_t idx);
	};
};