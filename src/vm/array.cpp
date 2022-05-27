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
	void array::gc_traverse(gc::stage_context s) {
		for (auto& e : *this) {
			if (e.is_gc())
				e.as_gc()->gc_tick(s);
		}
	}

	// Duplicates the array.
	//
	array* array::duplicate(vm* L) {
		array* r = L->alloc<array>();
		if (storage) {
			r->storage = L->alloc<array_store>(capacity() * sizeof(any));
			r->length  = length;
			memcpy(r->storage, storage, size() * sizeof(any));
		}
		return r;
	}

	// Reserve and resize.
	//
	void array::reserve(vm* L, size_t n) {
		if (n > capacity()) {
			size_t old_count    = size();
			size_t new_capacity = std::bit_ceil(n | 7);
			auto*  old_list     = begin();
			size_t alloc_length = sizeof(any) * new_capacity;
			storage             = L->alloc<array_store>(alloc_length);
			memcpy(storage, old_list, old_count * sizeof(any));
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