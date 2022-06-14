#include <vm/runtime.hpp>
#include <vm/array.hpp>
#include <vm/table.hpp>
#include <vm/string.hpp>

namespace li::runtime {
	// v--- Completely wrong.
	//
	any_t LI_CC field_set_raw(vm* L, any_t tbl, any_t key, any_t val) {
		if (key == nil) [[unlikely]] {
			return any(string::create(L, "indexing with null key"));
		}

		if (tbl.is_tbl()) {
			tbl.as_tbl()->set(L, key, val);
			//L->gc.tick(L); <--------- TODO:
		} else if (tbl.is_arr()) {
			if (!key.is_num() || key.as_num() < 0) [[unlikely]] {
				return any(string::create(L, "indexing array with non-integer or negative key"));
			}
			if (!tbl.as_arr()->set(L, msize_t(key.as_num()), val)) {
				return any(string::create(L, "out-of-boundaries array access"));
			}
		} else [[unlikely]] {
			return any(string::create(L, "indexing non-table"));
		}
		return {};  // ??
	}
	any_t LI_CC field_get_raw(vm* L, any_t tbl, any_t key) {
		if (key == nil) [[unlikely]] {
			util::abort("indexing with null key");
		}

		if (tbl.is_tbl()) {
			return tbl.as_tbl()->get(L, key);
		} else if (tbl.is_arr()) {
			if (!key.is_num() || key.as_num() < 0) [[unlikely]] {
				util::abort("indexing array with non-integer or negative key");
			}
			return tbl.as_arr()->get(L, msize_t(key.as_num()));
		} else if (tbl.is_str()) {
			if (!key.is_num() || key.as_num() < 0) [[unlikely]] {
				util::abort("indexing string with non-integer or negative key");
			}
			auto i = size_t(key.as_num());
			auto v = tbl.as_str()->view();
			return v.size() <= i ? nil : any(number((uint8_t) v[i]));
		} else {
			util::abort("indexing non-table");
		}
	}
};