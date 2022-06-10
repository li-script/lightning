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
	void gc::destroy(vm* L, table* o) {
		o->gc_destroy(L);
	}
	void gc::traverse(gc::stage_context s, table* o) {
		o->node_list->gc_tick(s);
		o->trait_traverse(s);
		traverse_n(s, (any*) o->begin(), 2 * o->realsize());
	}

	// Joins another table into this.
	//
	void table::join(vm* L, table* other) {
		if (!trait_seal) {
			trait_seal = other->trait_seal;
			trait_hide = other->trait_hide;
			trait_mask = other->trait_mask;
			traits     = other->traits;
		}
		if (!trait_freeze) {
			for (auto& [k, v] : *other) {
				if (k != nil)
					set(L, k, v);
			}
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
	LI_INLINE static bool set_if(table* t, any key, any value, size_t hash) {
		if (value == nil) {
			for (auto& entry : t->find(hash)) {
				if (entry.key == key) {
					entry = {nil, nil};
					t->active_count--;
					return true;
				}
			}
		}
		for (auto& entry : t->find(hash)) {
			if (entry.key == key) {
				entry.value     = value;
				return true;
			}
		}
		return false;
	}
	void table::set(vm* L, any key, any value) {
		size_t hash = key.hash();
		if (!set_if(this, key, value, hash)) {
			msize_t next_count = active_count + 1;
			while (true) {
				for (auto& entry : find(hash)) {
					if (entry.key == nil) {
						entry        = {key, value};
						active_count = next_count;
						return;
					}
				}
				resize(L, size() << 1);
			}
		}
	}
	any table::get(vm* L, any key) {
		any value = {};
		for (auto& entry : find(key.hash())) {
			if (entry.key == key)
				value = entry.value;
		}
		return value;
	}

	// Traitful table get/set.
	//
	std::pair<any, bool> table::tset(vm* L, any key, any value) {
		if (trait_freeze) [[unlikely]] {
			return {string::create(L, "modifying frozen table."), false};
		}

		if (!has_trait<trait::set>()) [[likely]] {
			set(L, key, value);
			return {nil, true};
		}

		L->push_stack(value);
		L->push_stack(key);
		auto ok = L->call(2, get_trait<trait::set>(), this);
		return {L->pop_stack(), ok};
	}
	std::pair<any, bool> table::tget(vm* L, any key) {
		auto result = get(L, key);
		if (result != nil || !has_trait<trait::get>()) [[likely]] {
			return {result, true};
		}

		auto get = get_trait<trait::get>();
		if (get.is_tbl()) {
			return {get.as_tbl()->get(L, key), true};
		} else {
			L->push_stack(key);
			auto ok = L->call(1, get, this);
			return {L->pop_stack(), ok};
		}
	}
};