#include <vm/array.hpp>

namespace li {
	array* array::create(vm* L, msize_t length, msize_t rsvd) {
		array* arr = L->alloc<array>();
		arr->storage = L->alloc<array_store>(std::bit_ceil(length + rsvd) * sizeof(any));
		arr->length  = length;
		fill_nil(arr->begin(), length);
		return arr;
	}

	// GC enumerator.
	//
	void gc::traverse(gc::stage_context s, array* o) {
		if (o->storage)
			o->storage->gc_tick(s);
		traverse_n(s, o->begin(), o->size());
	}

	// Joins another array into this.
	//
	void array::join(vm* L, array* other) {
		msize_t pos = size();
		resize(L, pos + other->size());
		memcpy(begin() + pos, other->begin(), other->size() * sizeof(any));
	}

	// Reserve and resize.
	//
	void array::reserve(vm* L, msize_t n) {
		if (!storage) {
			storage = L->alloc<array_store>(n * sizeof(any));
		} else if (msize_t c = capacity(); n > c) {
			msize_t new_capacity = std::max(n, c + (c >> 1));
			auto*  old_list     = storage;
			storage             = L->alloc<array_store>(sizeof(any) * new_capacity);
			memcpy(storage->entries, old_list->entries, c * sizeof(any));
			L->gc.free(L, old_list);
		}
	}
	void array::resize(vm* L, msize_t n) {
		msize_t old_count = size();
		if (n > old_count) {
			reserve(L, n);
			fill_nil(end(), (n - old_count));
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
			return nil;
		}
	}

	// Get/set.
	// - Set returns false if it should throw because of out-of-boundaries index.
	//
	bool array::set(vm* L, msize_t idx, any value) {
		if (idx < size()) {
			begin()[idx] = value;
			return true;
		}
		return false;
	}
	any array::get(vm* L, msize_t idx) {
		if (idx < size())
			return begin()[idx];
		else
			return nil;
	}
};