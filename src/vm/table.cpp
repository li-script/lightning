#include <vm/table.hpp>
#include <bit>
#include <vm/string.hpp>

namespace li {
	table* table::create(vm* L, size_t reserved_entry_count) {
		table* tbl = L->alloc<table>();
		if (reserved_entry_count) {
			tbl->resize(L, reserved_entry_count);
		}
		tbl->mask = tbl->compute_mask();
		return tbl;
	}

	// GC enumerator.
	//
	void table::gc_traverse(gc::stage_context s) {
		if (node_list)
			node_list->gc_tick(s);
		trait_traverse(s);
		for (auto& [k, v] : *this) {
			if (k.is_gc())
				k.as_gc()->gc_tick(s);
			if (v.is_gc())
				v.as_gc()->gc_tick(s);
		}
	}

	// Rehashing resize.
	//
	void table::resize(vm* L, size_t n) {
		size_t new_count = std::bit_ceil(n | (small_table_length - 1));
		size_t old_count = size();
		if (new_count > old_count) {
			auto*  old_list     = node_list;
			auto*  old_entries  = begin();
			size_t alloc_length = sizeof(table_entry) * (new_count + overflow_factor);
			node_list           = L->alloc<table_nodes>(alloc_length);
			mask                = (uint32_t) compute_mask();
			fill_none(node_list->entries, alloc_length / sizeof(any));

			if (old_entries) {
				for (size_t i = 0; i != (old_count + overflow_factor); i++) {
					if (old_entries[i].key != none) {
						set(L, old_entries[i].key, old_entries[i].value);
					}
				}
				if (old_list)
					L->gc.free(L, old_list);
			}
		}
	}

	// Raw table get/set.
	//
	LI_INLINE static bool set_if(table* t, any key, any value, size_t hash) {
		if (value == none) {
			for (auto& entry : t->find(hash)) {
				if (entry.key == key) {
					entry.key   = none;
					entry.value = none;
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
			uint32_t next_count = active_count + 1;
			while (true) {
				for (auto& entry : find(hash)) {
					if (entry.key == none) {
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
		for (auto& entry : find(key.hash())) {
			if (entry.key == key)
				return entry.value;
		}
		return {};
	}

	// Traitful table get/set.
	//
	std::pair<any, bool> table::tset(vm* L, any key, any value) {
		if (trait_freeze) {
			return {string::create(L, "modifying frozen table."), false};
		}

		if (!has_trait<trait::set>()) [[likely]] {
			set(L, key, value);
			return {none, true};
		}

		L->push_stack(value);
		L->push_stack(key);
		auto ok = L->scall(2, get_trait<trait::set>(), this);
		return {L->pop_stack(), ok};
	}
	std::pair<any, bool> table::tget(vm* L, any key) {
		auto result = get(L, key);
		if (result != none || !has_trait<trait::get>()) [[likely]] {
			return {result, true};
		}

		auto get = get_trait<trait::get>();
		if (get.is_tbl()) {
			return {get.as_tbl()->get(L, key), true};
		} else {
			L->push_stack(key);
			auto ok = L->scall(1, get, this);
			return {L->pop_stack(), ok};
		}
	}
};