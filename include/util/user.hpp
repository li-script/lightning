#pragma once
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <vm/function.hpp>

// Defines some user-facing helpers.
//
namespace li::util {
	// Global export helper handling nested names e.g. "mylib.test".
	//
	static void export_as(vm* L, std::string_view name, any value) {
		table* tbl = L->modules;

		while (true) {
			auto pos = name.find('.');
			if (pos == std::string::npos)
				break;

			auto base = name.substr(0, pos);
			name      = name.substr(pos + 1);
			auto key  = string::create(L, base);

			auto it = tbl->get(L, key);
			if (!it.is_tbl()) {
				auto ntbl = table::create(L, 1);
				ntbl->set_trait(L, trait::freeze, true);
				ntbl->set_trait(L, trait::seal, true);
				tbl->set(L, key, ntbl);
				it = ntbl;
			}
			tbl = it.as_tbl();
		}
		tbl->set(L, string::create(L, name), value);
	}
	static function* export_as(vm* L, std::string_view name, nfunc_t f) {
		auto vf = function::create(L, f);
		export_as(L, name, vf);
		return vf;
	}
};