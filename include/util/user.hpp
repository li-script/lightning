#pragma once
#include <vm/function.hpp>
#include <vm/rc.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>

// Defines some user-facing helpers.
//
namespace li::util {
	// Global export helper handling nested names e.g. "mylib.test".
	//
	static bool export_as(vm* L, std::string_view name, any value) {
		table* tbl = L->modules;

		while (true) {
			auto pos = name.find('.');
			if (pos == std::string::npos)
				break;

			auto base = name.substr(0, pos);
			name      = name.substr(pos + 1);
			any key   = string::create(L, base);

			any it = tbl->get(L, key);
			if (!it.is_tbl()) {
				auto ntbl       = table::create(L, 1);
				ntbl->is_frozen = true;
				if (!tbl->set(L, key, any(ntbl))) {
					rc::release(L, ntbl);
					rc::release(L, key);
					return false;
				}
				rc::release(L, ntbl);
				it = ntbl;
			}
			rc::release(L, key);
			tbl = it.as_tbl();
		}

		any  key     = string::create(L, name);
		bool success = tbl->set(L, key, value);
		rc::release(L, key);
		return success;
	}
	static function* export_as(vm* L, std::string_view name, nfunc_t f) {
		auto vf      = function::create(L, f);
		bool success = export_as(L, name, any(vf));
		rc::release(L, vf);
		return success ? vf : nullptr;
	}

	// Native function wrapper.
	//
	struct native_function : function {
		nfunc_info nfi;

		native_function(uint32_t attributes, const char* name, nfunc_t vinvoke, std::initializer_list<nfunc_overload> overloads = {}) {
			gc::make_non_gc(this);
			nfi.name = name;
			nfi.attr = attributes;
			ninfo    = &nfi;
			invoke   = vinvoke;
			std::copy(overloads.begin(), overloads.end(), nfi.overloads.begin());
		}

		void export_into(vm* L) { export_as(L, nfi.name, this); }
	};
};