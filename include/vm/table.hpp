#pragma once
#include <bit>
#include <span>
#include <vm/state.hpp>
#include <vm/traits.hpp>

namespace li {
	static constexpr msize_t small_table_length = 4;
	static constexpr msize_t overflow_factor    = 3;

	struct table_entry {
		any key;
		any value;
	};
	static constexpr msize_t table_hash_shift = 4;
	static_assert(sizeof(table_entry) == (1 << table_hash_shift), "Invalid constants.");

	struct table_nodes : gc::leaf<table_nodes> {
		table_entry entries[];
	};
	struct table : traitful_node<table, type_table> {
		static table* create(vm* L, msize_t reserved_entry_count = 0);

		table_nodes*  node_list = nullptr;
		table_entry   small_table[small_table_length + overflow_factor];
		msize_t       active_count = 0;
		size_t        mask         = 0;
		constexpr static size_t compute_mask(msize_t n) { return size_t(n - 1) << table_hash_shift; }

		table_entry* begin() { return node_list ? &node_list->entries[0] : &small_table[0]; }
		table_entry* end() { return begin() + realsize(); }
		msize_t      size() const { return msize_t((mask >> table_hash_shift) + 1); }
		msize_t      realsize() const { return size() + overflow_factor; }

		std::span<table_entry> find(size_t hash) {
			auto it = begin() + ((hash & mask) >> table_hash_shift);
			return {it, it + overflow_factor};
		}

		// Duplicates the table.
		//
		table* duplicate(vm* L) const {
			table* tbl     = L->duplicate(this);
			tbl->node_list = L->duplicate(tbl->node_list);
			return tbl;
		}

		// Joins another table into this.
		//
		void join(vm* L, table* other);

		// Rehashing resize.
		//
		void resize(vm* L, msize_t n);

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