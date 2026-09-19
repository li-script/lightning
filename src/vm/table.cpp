#include <bit>
#include <limits>
#include <vector>
#include <vm/rc.hpp>
#include <vm/shared.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>

namespace li {
	static bool table_key_equals(any_t lhs, any_t rhs) {
		if (lhs.value == rhs.value)
			return true;
		if (lhs.is_str() && rhs.is_str())
			return string_value_equals(lhs.as_str(), rhs.as_str());
		return canonicalize_zero_bits(lhs.value) == canonicalize_zero_bits(rhs.value);
	}

	static void check_mutation_version(vm* L, const table* value) {
		// Never wrap: an old iterator snapshot could otherwise become valid again.
		if (value->mutation_version == std::numeric_limits<uint64_t>::max()) [[unlikely]]
			L->panic("table mutation version overflow");
	}

	static std::span<table_entry> find_entries(table_nodes* nodes, size_t mask, size_t hash) {
		auto* it = nodes->entries + ((hash & mask) >> table_hash_shift);
		return {it, it + overflow_factor};
	}

	template<typename T>
	static T* allocate_table_part(vm* L, const table* owner, size_t extra = 0) {
		return owner->shared ? shared::allocate<T>(L, extra) : L->alloc<T>(extra);
	}

	static trait_set* clone_table_traits(vm* L, table* owner, const trait_set* source) {
		if (!source)
			return nullptr;
		if (!owner->shared)
			return clone_trait_set(L, source);

		trait_set* result = shared::allocate<trait_set>(L);
		result->seal      = source->seal;
		result->freeze    = source->freeze;
		result->hide      = source->hide;
		for (size_t i = 0; i != result->methods.size(); ++i) {
			function* method = source->methods[i];
			if (!method)
				continue;
			shared::prepared_value prepared = shared::prepare_store(L, owner, any(method));
			if (!prepared.ok || !prepared.value.is_fn() || !rc::try_retain(L, prepared.value)) {
				shared::finish_store(L, prepared);
				destroy_trait_set(L, result);
				return nullptr;
			}
			result->methods[i] = prepared.value.as_fn();
			shared::finish_store(L, prepared);
		}
		return result;
	}

	static void resize_unlocked(vm* L, table* self, msize_t n) {
		msize_t new_count = std::bit_ceil(n);
		msize_t old_count = self->size();
		if (new_count <= old_count)
			return;
		check_mutation_version(L, self);

		table_nodes* old_list    = self->node_list;
		table_entry* old_entries = old_list ? old_list->entries : nullptr;
		while (true) {
			const size_t alloc_length = sizeof(table_entry) * (new_count + overflow_factor);
			table_nodes* new_list     = allocate_table_part<table_nodes>(L, self, alloc_length);
			fill_nil(new_list->entries, alloc_length / sizeof(any));
			const size_t new_mask = table::compute_mask(new_count);
			bool         fits     = true;

			for (msize_t i = 0; i != old_count + overflow_factor; ++i) {
				if (old_entries[i].key == nil)
					continue;

				table_entry* destination = nullptr;
				for (table_entry& entry : find_entries(new_list, new_mask, old_entries[i].key.hash())) {
					if (entry.key == nil) {
						destination = &entry;
						break;
					}
				}
				if (!destination) {
					fits = false;
					break;
				}
				*destination = old_entries[i];
			}

			if (fits) {
				self->mask      = new_mask;
				self->node_list = new_list;
				++self->mutation_version;
				if (old_list)
					rc::release(L, old_list);
				return;
			}

			rc::release(L, new_list);
			new_count <<= 1;
		}
	}

	static bool set_unlocked(vm* L, table* self, any_t key, any_t value) {
		const size_t hash  = key.hash();
		table_entry* empty = nullptr;

		for (table_entry& entry : self->find(hash)) {
			if (table_key_equals(entry.key, key)) {
				check_mutation_version(L, self);
				if (entry.value.value == value.value) {
					++self->mutation_version;
					return true;
				}

				rc::retain(value);
				any previous = entry.value;
				entry.value  = value;
				++self->mutation_version;
				rc::release(L, previous);
				return true;
			}
			if (entry.key == nil && !empty)
				empty = &entry;
		}

		rc::retain(key);
		rc::retain(value);
		while (!empty) {
			resize_unlocked(L, self, self->size() << 1);
			for (table_entry& entry : self->find(hash)) {
				if (entry.key == nil) {
					empty = &entry;
					break;
				}
			}
		}

		check_mutation_version(L, self);
		empty->key   = key;
		empty->value = value;
		++self->active_count;
		++self->mutation_version;
		return true;
	}

	table* table::create(vm* L, msize_t rsvd) {
		rsvd                      = std::bit_ceil(rsvd | 2);
		size_t       alloc_length = sizeof(table_entry) * (rsvd + overflow_factor);
		table*       tbl          = L->alloc<table>();
		table_nodes* nl           = L->alloc<table_nodes>(alloc_length);
		tbl->mask                 = compute_mask(rsvd);
		tbl->node_list            = nl;
		fill_nil(nl->entries, alloc_length / sizeof(any));
		return tbl;
	}

	// Duplicates the table and retains its entries. A shared source produces a
	// shared table, shared descriptor, and shared node array.
	//
	table* table::duplicate(vm* L) const {
		if (!shared) {
			table* result            = L->alloc<table>();
			result->mask             = mask;
			result->active_count     = active_count;
			result->mutation_version = 0;
			result->traits           = clone_trait_set(L, traits);
			result->is_frozen        = is_frozen;
			result->rsvd             = rsvd;

			if (node_list) {
				const size_t alloc_length = sizeof(table_entry) * realsize();
				result->node_list         = L->alloc<table_nodes>(alloc_length);
				fill_nil(result->node_list->entries, alloc_length / sizeof(any));

				for (msize_t i = 0; i != realsize(); ++i) {
					const table_entry& source = node_list->entries[i];
					if (source.key == nil)
						continue;
					if (!rc::try_retain(L, source.key)) {
						rc::release(L, result);
						return nullptr;
					}
					if (!rc::try_retain(L, source.value)) {
						rc::release(L, source.key);
						rc::release(L, result);
						return nullptr;
					}
					result->node_list->entries[i] = source;
				}
			}
			return result;
		}

		shared::recursive_guard guard(L, const_cast<table*>(this));
		table*                  result = shared::allocate<table>(L);
		result->mask                   = mask;
		result->active_count           = active_count;
		result->mutation_version       = 0;
		result->is_frozen              = is_frozen;
		result->rsvd                   = rsvd;
		result->traits                 = clone_table_traits(L, result, traits);
		if (traits && !result->traits) {
			rc::release(L, result);
			return nullptr;
		}

		if (node_list) {
			const size_t alloc_length = sizeof(table_entry) * realsize();
			result->node_list         = shared::allocate<table_nodes>(L, alloc_length);
			fill_nil(result->node_list->entries, alloc_length / sizeof(any));

			for (msize_t i = 0; i != realsize(); ++i) {
				const table_entry& source = node_list->entries[i];
				if (source.key == nil)
					continue;
				if (!rc::try_retain(L, source.key)) {
					rc::release(L, result);
					return nullptr;
				}
				if (!rc::try_retain(L, source.value)) {
					rc::release(L, source.key);
					rc::release(L, result);
					return nullptr;
				}
				result->node_list->entries[i] = source;
			}
		}
		return result;
	}

	// RC destructor.
	//
	void gc::destroy(vm* L, table* o) {
		table_nodes* nodes = o->node_list;
		msize_t      count = nodes ? o->realsize() : 0;
		o->node_list       = nullptr;
		o->mask            = 0;
		o->active_count    = 0;
		destroy_trait_set(L, o->traits);

		for (msize_t i = 0; i != count; ++i) {
			table_entry& entry = nodes->entries[i];
			if (entry.key != nil) {
				any key   = entry.key;
				any value = entry.value;
				entry     = {nil, nil};
				rc::release(L, key);
				rc::release(L, value);
			}
		}
		if (nodes)
			rc::release(L, nodes);
	}

	// Joins a stable source snapshot. Every key/value is prepared before the
	// first destination mutation, so private mutable children cannot partially
	// enter a shared table.
	//
	bool table::join(vm* L, table* other) {
		if (other == this)
			return true;
		if (!shared && !other->shared) {
			std::vector<table_entry> entries;
			entries.reserve(other->active_count);
			for (const table_entry& entry : *other) {
				if (entry.key == nil)
					continue;
				if (!rc::try_retain(L, entry.key)) {
					for (const table_entry& retained : entries) {
						rc::release(L, retained.key);
						rc::release(L, retained.value);
					}
					return false;
				}
				if (!rc::try_retain(L, entry.value)) {
					rc::release(L, entry.key);
					for (const table_entry& retained : entries) {
						rc::release(L, retained.key);
						rc::release(L, retained.value);
					}
					return false;
				}
				entries.push_back(entry);
			}

			for (const table_entry& entry : entries)
				set_unlocked(L, this, entry.key, entry.value);
			for (const table_entry& entry : entries) {
				rc::release(L, entry.key);
				rc::release(L, entry.value);
			}
			return true;
		}

		std::vector<table_entry> entries;
		const auto               release_entries = [&]() {
			for (const table_entry& retained : entries) {
				rc::release(L, retained.key);
				rc::release(L, retained.value);
			}
		};
		const auto copy_source = [&]() -> bool {
			entries.reserve(other->active_count);
			for (const table_entry& entry : *other) {
				if (entry.key == nil)
					continue;

				shared::prepared_value prepared_key{};
				shared::prepared_value prepared_value{};
				any                    key   = entry.key;
				any                    value = entry.value;
				if (shared) {
					prepared_key = shared::prepare_store(L, this, key);
					if (!prepared_key.ok)
						return false;
					prepared_value = shared::prepare_store(L, this, value);
					if (!prepared_value.ok) {
						shared::finish_store(L, prepared_key);
						return false;
					}
					key   = prepared_key.value;
					value = prepared_value.value;
				}
				if (!rc::try_retain(L, key)) {
					if (shared) {
						shared::finish_store(L, prepared_key);
						shared::finish_store(L, prepared_value);
					}
					return false;
				}
				if (!rc::try_retain(L, value)) {
					rc::release(L, key);
					if (shared) {
						shared::finish_store(L, prepared_key);
						shared::finish_store(L, prepared_value);
					}
					return false;
				}
				entries.push_back({key, value});
				if (shared) {
					shared::finish_store(L, prepared_key);
					shared::finish_store(L, prepared_value);
				}
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
			release_entries();
			return false;
		}

		if (shared) {
			shared::recursive_guard destination_guard(L, this);
			for (const table_entry& entry : entries)
				set_unlocked(L, this, entry.key, entry.value);
		} else {
			for (const table_entry& entry : entries)
				set_unlocked(L, this, entry.key, entry.value);
		}
		release_entries();
		return true;
	}

	// Rehashing resize.
	//
	void table::resize(vm* L, msize_t n) {
		if (!shared) {
			resize_unlocked(L, this, n);
			return;
		}
		shared::recursive_guard guard(L, this);
		resize_unlocked(L, this, n);
	}

	// Raw table get/set.
	//
	bool table::set(vm* L, any_t key, any_t value) {
		LI_ASSERT(key != nil);
		if (!shared) {
			if (!rc::check_store(L, key) || !rc::check_store(L, value))
				return false;
			return set_unlocked(L, this, key, value);
		}

		shared::prepared_value prepared_key = shared::prepare_store(L, this, key);
		if (!prepared_key.ok)
			return false;
		shared::prepared_value prepared_value = shared::prepare_store(L, this, value);
		if (!prepared_value.ok) {
			shared::finish_store(L, prepared_key);
			return false;
		}

		shared::recursive_guard guard(L, this);
		const bool              stored = set_unlocked(L, this, prepared_key.value, prepared_value.value);
		shared::finish_store(L, prepared_key);
		shared::finish_store(L, prepared_value);
		return stored;
	}

	// Explicit deletion is distinct from storing nil.
	//
	bool table::erase(vm* L, any_t key) {
		if (key == nil)
			return false;
		const auto erase_unlocked = [&]() {
			for (table_entry& entry : find(key.hash())) {
				if (table_key_equals(entry.key, key)) {
					check_mutation_version(L, this);
					any old_key   = entry.key;
					any old_value = entry.value;
					entry         = {nil, nil};
					--active_count;
					++mutation_version;
					rc::release(L, old_key);
					rc::release(L, old_value);
					return true;
				}
			}
			return false;
		};
		if (!shared)
			return erase_unlocked();
		shared::recursive_guard guard(L, this);
		return erase_unlocked();
	}

	bool table::contains(any_t key) {
		if (key == nil)
			return false;
		const auto contains_unlocked = [&]() {
			for (table_entry& entry : find(key.hash())) {
				if (table_key_equals(entry.key, key))
					return true;
			}
			return false;
		};
		if (!shared)
			return contains_unlocked();
		shared::recursive_guard guard(nullptr, this);
		return contains_unlocked();
	}

	any_t table::get(vm* L, any_t key) {
		if (key == nil)
			return nil;
		const auto get_unlocked = [&]() -> any_t {
			for (table_entry& entry : find(key.hash())) {
				if (table_key_equals(entry.key, key))
					return entry.value;
			}
			return nil;
		};
		if (!shared)
			return get_unlocked();
		shared::recursive_guard guard(L, this);
		return get_unlocked();
	}
};
