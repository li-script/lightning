#pragma once
#include <bit>
#include <span>
#include <vm/state.hpp>

namespace lightning::core {
	static constexpr size_t small_table_length = 4;
	static constexpr size_t overflow_factor    = 3;

	struct table_entry {
		any key;
		any value;
	};
	struct table_nodes : gc_leaf<table_nodes> {
		table_entry entries[];
	};

	// Implemented inline as its templated.
	//
	struct table : gc_node<table> {
		static table* create(vm* L, size_t reserved_entry_count = 0);

		table_nodes* node_list = nullptr;
		table_entry  small_table[small_table_length + overflow_factor];

		// TODO: Metatable.

		table_entry*           begin() { return node_list ? &node_list->entries[0] : &small_table[0]; }
		table_entry*           end() { return begin() + size(); }
		size_t                 size() { return node_list ? (node_list->object_bytes() / sizeof(table_entry)) - overflow_factor : small_table_length; }
		size_t                 mask() { return size() - 1; }
		std::span<table_entry> find(size_t hash) {
			auto it = begin() + (hash & mask());
			return {it, it + overflow_factor};
		}

		// Duplicates the table.
		//
		table* duplicate(vm* L);

		// GC enumerator.
		//
		template<typename F>
		void enum_for_gc(F&& fn) {
			for (auto& [k, v] : *this) {
				if (k.is_gc())
					fn(k.as_gc());
				if (v.is_gc())
					fn(v.as_gc());
			}
		}

		// Rehashing resize.
		//
		void resize(vm* L, size_t n);

		// Table get/set.
		//
		void set(vm* L, any key, any value, bool assert_no_resize = false);
		any  get(vm* L, any key);
	};
};