#include <vm/table.hpp>
#include <bit>
#include <vm/string.hpp>

namespace li {
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

	// GC details.
	//
	void gc::traverse(gc::stage_context s, table* o) {
		o->node_list->gc_tick(s);
		traverse_n(s, (any*) o->begin(), 2 * o->realsize());
	}

	// Joins another table into this.
	//
	void table::join(vm* L, table* other) {
		for (auto& [k, v] : *other) {
			if (k != nil)
				set(L, k, v);
		}
	}

	// Rehashing resize.
	//
	void table::resize(vm* L, msize_t n) {
		msize_t new_count = std::bit_ceil(n);
		msize_t old_count = size();
		if (new_count > old_count) {
			auto*  old_list     = node_list;
			auto*  old_entries  = begin();
			size_t alloc_length = sizeof(table_entry) * (new_count + overflow_factor);
			auto*  new_list     = L->alloc<table_nodes>(alloc_length);
			mask                = compute_mask(new_count);
			node_list           = new_list;
			fill_nil(new_list->entries, alloc_length / sizeof(any));

			if (old_list) {
				for (msize_t i = 0; i != (old_count + overflow_factor); i++) {
					if (old_entries[i].key != nil) {
						set(L, old_entries[i].key, old_entries[i].value);
					}
				}
				L->gc.free(L, old_list);
			}
		}
	}

	// Raw table get/set.
	//
	void table::set(vm* L, any_t key, any_t value) {
		size_t hash = key.hash();
		if (value != nil) [[likely]] {
			msize_t      next_count   = active_count + 1;
			table_entry* suitable_kv = nullptr;
			for (auto& entry : find(hash)) {
				if (entry.key == key) {
					suitable_kv = &entry;
					next_count--;
					break;
				} else if (entry.key == nil) {
					suitable_kv = &entry;
				}
			}
			if (suitable_kv) [[likely]] {
				*suitable_kv = {key, value};
				active_count  = next_count;
				return;
			}

			while (true) {
				resize(L, size() << 1);
				for (auto& entry : find(hash)) {
					if (entry.key == nil) {
						entry        = {key, value};
						active_count = next_count;
						return;
					}
				}
			}
		} else {
			for (auto& entry : find(hash)) {
				if (entry.key == key) {
					entry = {nil, nil};
					active_count--;
					break;
				}
			}
		}
	}
	any_t table::get(vm* L, any_t key) {
		any value = {};
		for (auto& entry : find(key.hash())) {
			if (entry.key == key)
				value = entry.value;
		}
		return value;
	}
};