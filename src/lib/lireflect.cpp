#include <lang/parser.hpp>
#include <lang/typespec.hpp>
#include <lib/std.hpp>
#include <util/user.hpp>
#include <vm/array.hpp>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <vm/traits.hpp>

namespace li::lib {
	namespace {
		// Reflection results are immutable: installs the freeze and seal traits.
		// Returns false with the pending exception set on failure.
		bool seal_reflected(vm* L, any value) {
			for (trait flag : {trait::freeze, trait::seal}) {
				any result = set_trait(L, value, flag, any(true));
				if (result.is_exc())
					return false;
				rc::release(L, result);
			}
			return true;
		}

		// Resolves the declaration a reflection query names: a function, class,
		// struct, or an instance of one. Null when the value has no declaration.
		struct declaration_view {
			table*                  attributes = nullptr;
			const generic_template* generic    = nullptr;
			bool                    valid      = false;
		};
		declaration_view declaration_of(any value) {
			declaration_view view;
			if (value.is_fn()) {
				view.valid = true;
				if (auto* proto = value.as_fn()->proto) {
					view.attributes = proto->attributes;
					view.generic    = proto->generic.get();
				}
			} else if (value.is_vcl()) {
				view.valid      = true;
				view.attributes = value.as_vcl()->attributes;
				view.generic    = value.as_vcl()->generic.get();
			} else if (value.is_obj() && value.as_obj()->cl) {
				view.valid      = true;
				view.attributes = value.as_obj()->cl->attributes;
				view.generic    = value.as_obj()->cl->generic.get();
			}
			return view;
		}

		any_t LI_CC reflect_attributes(vm* L, any* args, slot_t count) {
			if (count != 1)
				return L->error("reflect.attributes expects one value");
			auto view = declaration_of(args[0]);
			if (!view.valid)
				return L->error("reflect.attributes expects a function, class, struct, or instance");
			if (view.attributes)
				return L->ok(any(view.attributes));

			auto* empty      = table::create(L);
			empty->is_frozen = true;
			if (!seal_reflected(L, any(empty))) {
				rc::release(L, empty);
				return exception_marker;
			}
			return L->take(any(empty));
		}

		const char* instantiation_status_name(generic_instantiation_status status) {
			switch (status) {
				case generic_instantiation_status::ok:
					return "Inst-OK";
				case generic_instantiation_status::cache_ok:
					return "InstCache-OK";
				case generic_instantiation_status::failed:
					return "Inst-Failed";
			}
			return "Inst-Failed";
		}

		// reflect.instantiations(value) -> [{types: [..], status: ".."}] (entries frozen).
		// Non-template declarations yield an empty array.
		any_t LI_CC reflect_instantiations(vm* L, any* args, slot_t count) {
			if (count != 1)
				return L->error("reflect.instantiations expects one value");
			auto view = declaration_of(args[0]);
			if (!view.valid)
				return L->error("reflect.instantiations expects a function, class, struct, or instance");

			array* records = array::create(L);
			auto   fail    = [&]() {
				rc::release(L, records);
				return exception_marker;
			};
			if (view.generic) {
				for (const auto& record : view.generic->instantiations) {
					array* types = array::create(L, 0, msize_t(record.types.size()));
					for (const auto& type : record.types) {
						string* spelled = string::create(L, strict::to_string(type));
						bool    pushed  = types->push(L, any(spelled));
						rc::release(L, spelled);
						if (!pushed) {
							rc::release(L, types);
							return fail();
						}
					}
					table*  entry      = table::create(L, 2);
					string* status     = string::create(L, instantiation_status_name(record.status));
					string* types_key  = string::create(L, "types");
					string* status_key = string::create(L, "status");
					bool    ok         = entry->set(L, any(types_key), any(types)) && entry->set(L, any(status_key), any(status));
					rc::release(L, status_key);
					rc::release(L, types_key);
					rc::release(L, status);
					rc::release(L, types);
					ok = ok && seal_reflected(L, any(entry)) && records->push(L, any(entry));
					rc::release(L, entry);
					if (!ok)
						return fail();
				}
			}
			// Arrays carry no traits; the result is a fresh copy, so mutation
			// cannot reach the template's own record list.
			return L->take(any(records));
		}
	}

	void register_reflect(vm* L) {
		util::export_as(L, "reflect.attributes", &reflect_attributes);
		util::export_as(L, "reflect.instantiations", &reflect_instantiations);
	}
}
