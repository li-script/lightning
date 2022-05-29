#include <vm/table.hpp>
#include <bit>

namespace li {
	table* table::create(vm* L, size_t reserved_entry_count) {
		table* tbl = L->alloc<table>();
		if (reserved_entry_count) {
			tbl->resize(L, reserved_entry_count);
		}
		return tbl;
	}

	// GC enumerator.
	//
	void table::gc_traverse(gc::stage_context s) {
		if (node_list)
			node_list->gc_tick(s);
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
	void table::set(vm* L, any key, any value) {
		size_t hash = key.hash();

		if (value == none) {
			for (auto& entry : find(key.hash())) {
				if (entry.key == key) {
					entry.key   = none;
					entry.value = none;
					active_count--;
					break;
				}
			}
			return;
		}

		uint32_t next_count = active_count + 1;

		while (true) {
			auto range = find(hash);
			for (auto& entry : range) {
				if (entry.key == key) {
					entry.value  = value;
					active_count = next_count - 1;
					return;
				}
			}
			for (auto& entry : range) {
				if (entry.key == none) {
					entry        = {key, value};
					active_count = next_count;
					return;
				}
			}
			resize(L, size() << 1);
		}
	}
	any table::get(vm* L, any key) {
		for (auto& entry : find(key.hash())) {
			if (entry.key == key)
				return entry.value;
		}
		return {};
	}
};