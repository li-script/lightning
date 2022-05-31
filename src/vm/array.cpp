#include <vm/array.hpp>

namespace li {
	array* array::create(vm* L, size_t reserved_entry_count) {
		array* arr = L->alloc<array>();
		if (reserved_entry_count) {
			arr->reserve(L, reserved_entry_count);
		}
		return arr;
	}

	// GC enumerator.
	//
	void gc::traverse(gc::stage_context s, array* o) {
		if (o->storage)
			o->storage->gc_tick(s);
		traverse_n(s, o->begin(), o->size());
	}

	// Reserve and resize.
	//
	void array::reserve(vm* L, size_t n) {
		if (!storage) {
			storage = L->alloc<array_store>(n);
		} else if (size_t c = capacity(); n > c) {
			size_t new_capacity = std::max(n, c + (c >> 1));
			auto*  old_list     = storage;
			storage             = L->alloc<array_store>(sizeof(any) * new_capacity);
			memcpy(storage->entries, old_list->entries, c * sizeof(any));
			L->gc.free(L, old_list);
		}
	}
	void array::resize(vm* L, size_t n) {
		size_t old_count = size();
		if (n > old_count) {
			reserve(L, n);
			fill_none(end(), (n - old_count));
		}
		length = n;
	}

	// Push-back.
	//
	void array::push(vm* L, any value) {
		if (size() == capacity()) [[unlikely]] {
			reserve(L, size() + 1);
		}
		begin()[length++] = value;
	}

	// Pop-back.
	//
	any array::pop() {
		if (size() != 0) {
			return begin()[--length];
		} else {
			return none;
		}
	}

	// Get/set.
	// - Set returns false if it should throw because of out-of-boundaries index.
	//
	bool array::set(vm* L, size_t idx, any value) {
		if (idx < size()) {
			begin()[idx] = value;
			return true;
		}
		return false;
	}
	any array::get(vm* L, size_t idx) {
		if (idx < size())
			return begin()[idx];
		else
			return none;
	}
};