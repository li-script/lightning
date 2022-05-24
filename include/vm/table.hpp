#pragma once
#include <vm/state.hpp>
#include <span>

namespace lightning::core {
	static constexpr size_t small_table_length =  4;
	static constexpr size_t overflow_factor =     3;

	struct table_entry {
		any key;
		any value;
	};
	struct table_nodes : gc_leaf<table_nodes> {
		table_entry entries[];
	};

	struct table : gc_node<table> {
		static table* create(vm* L, size_t reserved_entry_count = 0);

		table_nodes* node_list = nullptr;
		size_t       node_mask = 0;
		table_entry  small_table[small_table_length];

		// TODO: Metatable.

		table_entry*           begin() { return node_list ? &node_list->entries[0] : &small_table[0]; }
		table_entry*           end() { return begin() + size(); }
		size_t                 size() { return node_list ? node_mask + 1 : small_table_length; }
		size_t                 mask() { return node_list ? node_mask : small_table_length - 1; }
		std::span<table_entry> find(size_t hash) {
			auto it = begin() + (hash & mask());
			return {it, it + overflow_factor};
		}

		void resize(vm* L, size_t n);
		void set(vm* L, any key, any value, bool assert_no_resize = false);
		any  get(vm* L, any key);
	};
};