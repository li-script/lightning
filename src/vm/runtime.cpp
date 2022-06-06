#include <vm/runtime.hpp>
#include <vm/array.hpp>
#include <vm/table.hpp>
#include <vm/string.hpp>

namespace li::runtime {
	array* LI_C_CC array_new(vm* L, msize_t n) {
		return array::create(L, n, 0);
	}
	table* LI_C_CC table_new(vm* L, msize_t n) {
		return table::create(L, n);
	}

	uint64_t LI_C_CC field_set_raw(vm* L, uint64_t _unk, uint64_t _key, uint64_t _value) {
		any tbl(std::in_place, _unk);
		any key(std::in_place, _key);
		any val(std::in_place, _value);

		if (key == none) [[unlikely]] {
			return any(string::create(L, "indexing with null key")).value;
		} /* else if (tbl == none) [[unlikely]] { <------- TODO:
			tbl = any{table::create(L)};
		}*/

		if (tbl.is_tbl()) {
			if (tbl.as_tbl()->trait_freeze) [[unlikely]] {
				return any(string::create(L, "modifying frozen table.")).value;
			}
			tbl.as_tbl()->set(L, key, val);
			//L->gc.tick(L); <--------- TODO:
		} else if (tbl.is_arr()) {
			if (!key.is_num() || key.as_num() < 0) [[unlikely]] {
				return any(string::create(L, "indexing array with non-integer or negative key")).value;
			}
			if (!tbl.as_arr()->set(L, msize_t(key.as_num()), val)) {
				return any(string::create(L, "out-of-boundaries array access")).value;
			}
		} else [[unlikely]] {
			return any(string::create(L, "indexing non-table")).value;
		}
		return 0;
	}

	uint64_t LI_C_CC field_get_raw(vm* L, uint64_t _unk, uint64_t _key) {
		any tbl(std::in_place, _unk);
		any key(std::in_place, _key);
		if (key == none) [[unlikely]] {
			// VM_RET(string::create(L, "indexing with null key"), true);
			util::abort("indexing with null key");
		}

		if (tbl.is_tbl()) {
			return tbl.as_tbl()->get(L, key).value;
		} else if (tbl.is_arr()) {
			if (!key.is_num() || key.as_num() < 0) [[unlikely]] {
				util::abort("indexing array with non-integer or negative key");
			}
			return tbl.as_arr()->get(L, msize_t(key.as_num())).value;
		} else if (tbl == none) {
			return none.value;
		} else {
			util::abort("indexing non-table");
		}
	}
};