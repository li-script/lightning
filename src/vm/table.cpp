#include <vm/table.hpp>
#include <bit>

namespace lightning::core {
	table* table::create(vm* L, size_t reserved_entry_count) {
		table* tbl = L->alloc<table>();
		if (reserved_entry_count) {
			tbl->resize(L, reserved_entry_count);
		}
		return tbl;
	}

	// Duplicates the table.
	//
	table* table::duplicate(vm* L) {
		table* tbl = L->alloc<table>();
		if (auto* pnodes = tbl->node_list) {
			size_t len    = pnodes->object_bytes();
			auto   nnodes = L->alloc<table_nodes>(len);
			memcpy(nnodes->entries, pnodes->entries, len);
			tbl->node_list = nnodes;
		} else {
			memcpy(tbl->small_table, small_table, sizeof(small_table));
		}
		return tbl;
	}

	// Rehashing resize.
	//
	void table::resize(vm* L, size_t n) {
		size_t new_count = std::bit_ceil(n | (small_table_length - 1));
		size_t old_count = size();
		if (new_count > old_count) {
			auto*  old_list = begin();
			size_t alloc_length   = sizeof(typename table_entry) * (new_count + overflow_factor);
			node_list           = L->alloc<table_nodes>(alloc_length);
			memset(node_list->entries, 0xFF, alloc_length);

			if (old_list) {
				// TODO: Free old?
				for (size_t i = 0; i != old_count; i++) {
					if (old_list[i].key != none) {
						set(L, old_list[i].key, old_list[i].value, true);
					}
				}
			}
		}
	}

	// Table get/set.
	//
	void table::set(vm* L, any key, any value, bool assert_no_resize) {
		size_t hash = key.hash();

		while (true) {
			auto range = find(hash);
			for (auto& entry : range) {
				if (entry.key == key) {
					entry.value = value;
					return;
				}
			}
			for (auto& entry : range) {
				if (entry.key == none) {
					entry = {key, value};
					return;
				}
			}

			LI_ASSERT(!assert_no_resize);
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