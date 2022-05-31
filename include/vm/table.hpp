#pragma once
#include <bit>
#include <span>
#include <vm/state.hpp>
#include <vm/traits.hpp>

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
	struct table : traitful_node<table, type_table> {
		static table* create(vm* L, size_t reserved_entry_count = 0);

		table_nodes*  node_list = nullptr;
		table_entry   small_table[small_table_length + overflow_factor];
		uint32_t      active_count = 0;
		uint32_t      mask         = 0;

		table_entry*           begin() { return node_list ? &node_list->entries[0] : &small_table[0]; }
		table_entry*           end() { return begin() + size() + overflow_factor; }
		size_t                 size() const { return node_list ? std::bit_floor((node_list->object_bytes() / sizeof(table_entry)) - overflow_factor) : small_table_length; }
		size_t                 compute_mask() const { return size() - 1; }
		std::span<table_entry> find(size_t hash) {
			auto it = begin() + (hash & mask);
			return {it, it + overflow_factor};
		}

		// Duplicates the table.
		//
		table* duplicate(vm* L) const {
			table* tbl     = L->duplicate(this);
			tbl->node_list = L->duplicate(tbl->node_list);
			return tbl;
		}

		// Rehashing resize.
		//
		void resize(vm* L, size_t n);

		// Raw table get/set.
		//
		void set(vm* L, any key, any value);
		any  get(vm* L, any key);

		// Traitful table get/set.
		//
		std::pair<any, bool> tset(vm* L, any key, any value);
		std::pair<any, bool> tget(vm* L, any key);
	};
};