#include <limits>
#include <vector>
#include <vm/array.hpp>
#include <vm/rc.hpp>
#include <vm/shared.hpp>

namespace li {
	static size_t checked_storage_bytes(vm* L, msize_t count) {
		if (size_t(count) > std::numeric_limits<size_t>::max() / sizeof(any))
			L->panic("array allocation size overflow");
		return size_t(count) * sizeof(any);
	}

	template<typename T>
	static T* allocate_array_part(vm* L, const array* owner, size_t extra = 0) {
		return owner->shared ? shared::allocate<T>(L, extra) : L->alloc<T>(extra);
	}

	static void reserve_unlocked(vm* L, array* self, msize_t n) {
		const msize_t old_capacity = self->capacity();
		if (n <= old_capacity)
			return;

		const msize_t growth         = old_capacity >> 1;
		const msize_t grown_capacity = old_capacity > std::numeric_limits<msize_t>::max() - growth ? std::numeric_limits<msize_t>::max() : old_capacity + growth;
		const msize_t new_capacity   = old_capacity ? std::max(n, grown_capacity) : n;
		array_store*  new_storage    = allocate_array_part<array_store>(L, self, checked_storage_bytes(L, new_capacity));
		fill_nil(new_storage->entries, msize_t(new_storage->object_bytes() / sizeof(any)));
		if (self->storage)
			memcpy(new_storage->entries, self->storage->entries, size_t(self->length) * sizeof(any));

		array_store* old_storage = self->storage;
		self->storage            = new_storage;
		++self->mutation_version;
		if (old_storage)
			rc::release(L, old_storage);
	}

	static void resize_unlocked(vm* L, array* self, msize_t n) {
		const msize_t old_count = self->size();
		if (n == old_count)
			return;

		if (n > old_count) {
			reserve_unlocked(L, self, n);
			fill_nil(self->begin() + old_count, n - old_count);
			self->length = n;
			++self->mutation_version;
			return;
		}

		array* retired   = allocate_array_part<array>(L, self);
		retired->storage = self->storage;
		retired->length  = old_count;

		const msize_t old_capacity = self->capacity();
		array_store*  replacement  = old_capacity ? allocate_array_part<array_store>(L, self, checked_storage_bytes(L, old_capacity)) : nullptr;
		if (replacement)
			fill_nil(replacement->entries, msize_t(replacement->object_bytes() / sizeof(any)));
		for (msize_t i = 0; i != n; ++i) {
			any value = self->begin()[i];
			rc::retain(value);
			replacement->entries[i] = value;
		}

		self->storage = replacement;
		self->length  = n;
		++self->mutation_version;
		rc::release(L, retired);
	}

	array* array::create(vm* L, msize_t length, msize_t rsvd) {
		if (rsvd > std::numeric_limits<msize_t>::max() - length)
			L->panic("array capacity overflow");

		const msize_t requested_capacity = length + rsvd;
		const msize_t rounded_limit      = std::bit_floor(std::numeric_limits<msize_t>::max());
		const msize_t actual_capacity    = requested_capacity <= rounded_limit ? std::bit_ceil(requested_capacity) : requested_capacity;

		array* arr   = L->alloc<array>();
		arr->storage = L->alloc<array_store>(checked_storage_bytes(L, actual_capacity));
		arr->length  = length;
		fill_nil(arr->begin(), arr->capacity());
		return arr;
	}

	// Duplicates the array and retains its entries. Shared arrays stay shared so
	// their backing store can never migrate into a caller's private heap.
	//
	array* array::duplicate(vm* L) const {
		if (!shared) {
			array* result  = L->alloc<array>();
			result->length = length;
			if (storage) {
				result->storage = L->alloc<array_store>(checked_storage_bytes(L, capacity()));
				fill_nil(result->begin(), result->capacity());
				for (msize_t i = 0; i != length; ++i) {
					any value = storage->entries[i];
					if (!rc::try_retain(L, value)) {
						rc::release(L, result);
						return nullptr;
					}
					result->begin()[i] = value;
				}
			}
			return result;
		}

		shared::recursive_guard guard(L, const_cast<array*>(this));
		array*                  result = shared::allocate<array>(L);
		result->length                 = length;
		if (storage) {
			result->storage = shared::allocate<array_store>(L, checked_storage_bytes(L, capacity()));
			fill_nil(result->begin(), result->capacity());
			for (msize_t i = 0; i != length; ++i) {
				any value = storage->entries[i];
				if (!rc::try_retain(L, value)) {
					rc::release(L, result);
					return nullptr;
				}
				result->begin()[i] = value;
			}
		}
		return result;
	}

	// RC destructor.
	//
	void gc::destroy(vm* L, array* o) {
		array_store* storage = o->storage;
		msize_t      length  = o->length;
		o->storage           = nullptr;
		o->length            = 0;

		if (storage) {
			for (msize_t i = 0; i != length; ++i) {
				any value           = storage->entries[i];
				storage->entries[i] = nil;
				rc::release(L, value);
			}
			rc::release(L, storage);
		}
	}

	// Joins a stable snapshot of another array into this array. Preparing every
	// source value before the destination mutation makes rejection atomic.
	//
	bool array::join(vm* L, array* other) {
		if (!shared && !other->shared) {
			const msize_t pos        = size();
			const msize_t other_size = other->size();
			if (other_size > std::numeric_limits<msize_t>::max() - pos) {
				L->error("array length overflow");
				return false;
			}
			const any* source = other->begin();

			msize_t retained = 0;
			for (; retained != other_size; ++retained) {
				if (!rc::try_retain(L, source[retained])) {
					while (retained != 0)
						rc::release(L, source[--retained]);
					return false;
				}
			}

			resize_unlocked(L, this, pos + other_size);
			source = other->begin();
			for (msize_t i = 0; i != other_size; ++i)
				begin()[pos + i] = source[i];
			return true;
		}

		std::vector<any> snapshot;
		const auto       release_snapshot = [&]() {
			for (any value : snapshot)
				rc::release(L, value);
		};
		const auto copy_source = [&]() -> bool {
			snapshot.reserve(other->length);
			for (msize_t i = 0; i != other->length; ++i) {
				any value = other->begin()[i];
				if (!rc::try_retain(L, value))
					return false;
				snapshot.emplace_back(value);
			}
			return true;
		};

		bool copied;
		if (other->shared) {
			shared::recursive_guard source_guard(L, other);
			copied = copy_source();
		} else {
			copied = copy_source();
		}
		if (!copied) {
			release_snapshot();
			return false;
		}

		// Source references pin the raw snapshot after its lock is released. Only
		// then validate/copy against the destination, before taking its lock.
		if (shared) {
			for (any& value : snapshot) {
				shared::prepared_value prepared = shared::prepare_store(L, this, value);
				if (!prepared.ok) {
					release_snapshot();
					return false;
				}
				if (!rc::try_retain(L, prepared.value)) {
					shared::finish_store(L, prepared);
					release_snapshot();
					return false;
				}
				rc::release(L, value);
				value = prepared.value;
				shared::finish_store(L, prepared);
			}
		}

		const auto append = [&]() -> bool {
			const msize_t pos = size();
			if (snapshot.size() > size_t(std::numeric_limits<msize_t>::max() - pos)) {
				L->error("array length overflow");
				return false;
			}
			resize_unlocked(L, this, pos + msize_t(snapshot.size()));
			for (msize_t i = 0; i != snapshot.size(); ++i)
				begin()[pos + i] = snapshot[i];
			return true;
		};

		bool appended;
		if (shared) {
			shared::recursive_guard destination_guard(L, this);
			appended = append();
		} else {
			appended = append();
		}
		if (!appended)
			release_snapshot();
		return appended;
	}

	// Reserve, resize, and fill.
	//
	void array::reserve(vm* L, msize_t n) {
		if (!shared) {
			reserve_unlocked(L, this, n);
			return;
		}
		shared::recursive_guard guard(L, this);
		reserve_unlocked(L, this, n);
	}

	void array::resize(vm* L, msize_t n) {
		if (!shared) {
			resize_unlocked(L, this, n);
			return;
		}
		shared::recursive_guard guard(L, this);
		resize_unlocked(L, this, n);
	}

	bool array::fill(vm* L, any_t value, msize_t start, msize_t end) {
		if (!shared) {
			LI_ASSERT(start <= end && end <= length);
			if (start == end)
				return true;
			if (!rc::check_store(L, value))
				return false;

			array* replacement   = L->alloc<array>();
			replacement->length  = length;
			const msize_t count  = capacity();
			replacement->storage = count ? L->alloc<array_store>(checked_storage_bytes(L, count)) : nullptr;
			if (replacement->storage)
				fill_nil(replacement->begin(), replacement->capacity());

			for (msize_t i = 0; i != length; ++i) {
				any selected = start <= i && i < end ? any(value) : begin()[i];
				if (!rc::try_retain(L, selected)) {
					rc::release(L, replacement);
					return false;
				}
				replacement->begin()[i] = selected;
			}

			std::swap(storage, replacement->storage);
			++mutation_version;
			rc::release(L, replacement);
			return true;
		}

		shared::prepared_value prepared = shared::prepare_store(L, this, value);
		if (!prepared.ok)
			return false;
		shared::recursive_guard guard(L, this);
		if (start > end || end > length) {
			shared::finish_store(L, prepared);
			L->error("out-of-boundaries array fill");
			return false;
		}
		if (start == end) {
			shared::finish_store(L, prepared);
			return true;
		}

		array* replacement   = shared::allocate<array>(L);
		replacement->length  = length;
		const msize_t count  = capacity();
		replacement->storage = count ? shared::allocate<array_store>(L, checked_storage_bytes(L, count)) : nullptr;
		if (replacement->storage)
			fill_nil(replacement->begin(), replacement->capacity());

		for (msize_t i = 0; i != length; ++i) {
			any selected = start <= i && i < end ? prepared.value : begin()[i];
			if (!rc::try_retain(L, selected)) {
				rc::release(L, replacement);
				shared::finish_store(L, prepared);
				return false;
			}
			replacement->begin()[i] = selected;
		}

		std::swap(storage, replacement->storage);
		++mutation_version;
		rc::release(L, replacement);
		shared::finish_store(L, prepared);
		return true;
	}

	// Push-back.
	//
	bool array::push(vm* L, any value) {
		if (!shared) {
			if (!rc::try_retain(L, value))
				return false;
			if (size() == capacity()) [[unlikely]]
				reserve_unlocked(L, this, size() + 1);
			begin()[length++] = value;
			++mutation_version;
			return true;
		}

		shared::prepared_value prepared = shared::prepare_store(L, this, value);
		if (!prepared.ok)
			return false;
		shared::recursive_guard guard(L, this);
		if (!rc::try_retain(L, prepared.value)) {
			shared::finish_store(L, prepared);
			return false;
		}
		if (size() == capacity()) [[unlikely]]
			reserve_unlocked(L, this, size() + 1);
		begin()[length++] = prepared.value;
		++mutation_version;
		shared::finish_store(L, prepared);
		return true;
	}

	// Pop-back.
	//
	any array::pop() {
		if (!shared) {
			if (size() == 0)
				return nil;
			any value       = begin()[--length];
			begin()[length] = nil;
			++mutation_version;
			return value;
		}

		shared::recursive_guard guard(nullptr, this);
		if (size() == 0)
			return nil;
		any value       = begin()[--length];
		begin()[length] = nil;
		++mutation_version;
		return value;
	}

	// Get/set.
	// - Set returns false if it should throw because of out-of-boundaries index.
	//
	bool array::set(vm* L, msize_t idx, any value) {
		if (!shared) {
			if (idx >= size() || !rc::check_store(L, value))
				return false;

			any& slot = begin()[idx];
			if (slot == value) {
				++mutation_version;
				return true;
			}

			rc::retain(value);
			any previous = slot;
			slot         = value;
			++mutation_version;
			rc::release(L, previous);
			return true;
		}

		shared::prepared_value prepared = shared::prepare_store(L, this, value);
		if (!prepared.ok)
			return false;
		shared::recursive_guard guard(L, this);
		if (idx >= size()) {
			shared::finish_store(L, prepared);
			return false;
		}

		any& slot = begin()[idx];
		if (slot == prepared.value) {
			++mutation_version;
			shared::finish_store(L, prepared);
			return true;
		}
		if (!rc::try_retain(L, prepared.value)) {
			shared::finish_store(L, prepared);
			return false;
		}
		any previous = slot;
		slot         = prepared.value;
		++mutation_version;
		rc::release(L, previous);
		shared::finish_store(L, prepared);
		return true;
	}
	any array::get(vm* L, msize_t idx) {
		if (!shared)
			return idx < size() ? begin()[idx] : any(nil);
		shared::recursive_guard guard(L, this);
		return idx < size() ? begin()[idx] : any(nil);
	}
};
