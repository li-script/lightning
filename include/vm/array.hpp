#pragma once
#include <bit>
#include <span>
#include <vm/state.hpp>

namespace li {
	struct array_store : gc::leaf<array_store> {
		any entries[];
	};
	struct array : gc::node<array, type_array> {
		static array* create(vm* L, msize_t length = 0, msize_t rsvd = 0);

		array_store* storage          = nullptr;
		msize_t      length           = 0;
		uint64_t     mutation_version = 0;
		any*         begin() { return storage ? storage->entries : nullptr; }
		any*         end() { return begin() + size(); }
		msize_t      size() const { return length; }
		msize_t      capacity() const { return storage ? msize_t(storage->object_bytes() / sizeof(any)) : 0; }

		// Duplicates the array.
		//
		array* duplicate(vm* L) const;

		// Joins another array into this.
		//
		bool join(vm* L, array* other);

		// Reserve, resize, and fill.
		//
		void reserve(vm* L, msize_t n);
		void resize(vm* L, msize_t n);
		bool fill(vm* L, any_t value, msize_t start, msize_t end);

		// Push-back.
		//
		bool push(vm* L, any value);

		// Pop-back.
		//
		any pop();

		// Get/set.
		// - Set returns false if it should throw because of out-of-boundaries index.
		//
		bool set(vm* L, msize_t idx, any value);
		any  get(vm* L, msize_t idx);
	};
};