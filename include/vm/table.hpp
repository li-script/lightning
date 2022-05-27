#pragma once
#include <bit>
#include <span>
#include <vm/state.hpp>

namespace li {
	static constexpr size_t small_table_length = 4;
	static constexpr size_t overflow_factor    = 3;

	struct table_entry {
		any key;
		any value;
	};
	struct table_nodes : gc::leaf<table_nodes> {
		table_entry entries[];
	};
	struct table : gc::node<table, type_table> {
		static table* create(vm* L, size_t reserved_entry_count = 0);

		table_nodes* node_list = nullptr;
		table_entry  small_table[small_table_length + overflow_factor];

		// TODO: Metatable.

		table_entry*           begin() { return node_list ? &node_list->entries[0] : &small_table[0]; }
		table_entry*           end() { return begin() + size() + overflow_factor; }
		size_t                 size() const { return node_list ? std::bit_floor((node_list->object_bytes() / sizeof(table_entry)) - overflow_factor) : small_table_length; }
		size_t                 mask() const { return size() - 1; }
		std::span<table_entry> find(size_t hash) {
			auto it = begin() + (hash & mask());
			return {it, it + overflow_factor};
		}

		// Duplicates the table.
		//
		table* duplicate(vm* L);

		// GC enumerator.
		//
		void gc_traverse(gc::stage_context s) override;

		// Rehashing resize.
		//
		void resize(vm* L, size_t n);

		// Table get/set.
		//
		void set(vm* L, any key, any value, bool assert_no_resize = false);
		any  get(vm* L, any key);
	};
};