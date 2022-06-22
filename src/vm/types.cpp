#include <vm/types.hpp>
#include <vm/table.hpp>
#include <vm/array.hpp>
#include <vm/object.hpp>
#include <vm/string.hpp>
#include <lib/std.hpp>
#include <util/enuminfo.hpp>

namespace li {
	std::string_view get_type_name(vm* L, type vt) {
		if (vt >= type::obj) {
			return util::name_enum(vt);
		} else if (L) {
			vclass* c = typeset_fetch(L, vt);
			if (!c)
				return "dangling class";
			return c->name->view();
		} else {
			return "unknown class";
		}
	}

	// Full type getter and type namer.
	//
	const char* any_t::type_name() const {
		if (is_obj())
			return as_obj()->cl->name->c_str();
		else
			return type_names[type()];
	}

	li::type any_t::xtype() const {
		if (is_obj())
			return li::type(as_obj()->type_id);
		else
			return to_type(type());
	}

	// String coercion.
	//
	template<typename F>
	static void format_any(any_t a, F&& formatter) {
		switch (a.type()) {
			case type_nil:
				formatter("nil");
				break;
			case type_bool:
				formatter(a.as_bool() ? "true" : "false");
				break;
			case type_number:
				if (a.as_num() == (int64_t) a.as_num()) {
					formatter("%.0lf", a.as_num());
				} else {
					formatter("%lf", a.as_num());
				}
				break;
			case type_array:
				formatter("array @ %p", a.as_gc());
				break;
			case type_table:
				formatter("table @ %p", a.as_gc());
				break;
			case type_string:
				formatter("\"%s\"", a.as_str()->data);
				break;
			case type_object:
				formatter("%s @ %p", a.as_obj()->cl->name->c_str(), a.as_gc());
				break;
			case type_class:
				formatter("class %s @ %p", a.as_vcl()->name->c_str(), a.as_gc());
				break;
			case type_function:
				if (a.as_fn()->is_virtual())
					formatter("function @ %s", a.as_fn()->proto->src_chunk->c_str());
				else if (auto nf = a.as_fn()->ninfo; nf && nf->name)
					formatter(nf->name);
				else
					formatter("native-function @ %p", a.as_fn());
				break;
			case type_exception:
				formatter("((exception marker))");
				break;
			default:
				util::abort("invalid type");
				break;
		}
	};
	string* any_t::to_string(vm* L) const {
		if (is_str()) [[likely]]
			return as_str();
		string* result;
		format_any(*this, [&] <typename... Tx> (const char* fmt, Tx&&... args) {
			if constexpr (sizeof...(Tx) == 0) {
				result = string::create(L, fmt);
			} else {
				result = string::format(L, fmt, std::forward<Tx>(args)...);
			}
		});
		return result;
	}
	std::string any_t::to_string() const {
		if (is_str())
			return std::string{as_str()->view()};
		std::string result;
		format_any(*this, [&]<typename... Tx>(const char* fmt, Tx&&... args) {
			if constexpr (sizeof...(Tx) == 0) {
				result = fmt;
			} else {
				result = util::fmt(fmt, std::forward<Tx>(args)...);
			}
		});
		return result;
	}
	void any_t::print() const {
		if (is_str()) [[likely]]
			return (void) fputs(as_str()->c_str(), stdout);
		format_any(*this, [&]<typename... Tx>(const char* fmt, Tx&&... args) {
			if constexpr (sizeof...(Tx) == 0) {
				fputs(fmt, stdout);
			} else {
				printf(fmt, std::forward<Tx>(args)...);
			}
		});
	}

	// Number coercion.
	//
	number any_t::coerce_num() const {
		if (is_num()) [[likely]]
			return as_num();
		switch (type()) {
			case type_bool:
				return as_bool() ? 1 : 0;
			case type_nil:
				return 0;
			default:
				return 1;
			case type_string: {
				char* end;
				return strtod(as_str()->c_str(), &end);
			}
		}
	}

	// Duplication.
	//
	any_t any_t::duplicate(vm* L) const {
		if (is_arr()) {
			return any(as_arr()->duplicate(L));
		} else if (is_tbl()) {
			return any(as_tbl()->duplicate(L));
		} else if (is_obj()) {
			return any(as_obj()->duplicate(L));
		} else if (is_fn()) {
			return any(as_fn()->duplicate(L));
		} else {
			return *this;
		}
	}
	// Constructs default value of the data type.
	//
	any any::make_default(vm* L, li::type t) {
		if (t < type::obj) {
			vclass* c = typeset_fetch(L, t);
			LI_ASSERT(c != nullptr);
			return object::create(L, c);
		}
		switch (t) {
			case type::tbl:
				return table::create(L);
			case type::arr:
				return array::create(L);
			case type::fn:
				return &lib::detail::builtin_null_function;
			case type::str:
				return L->empty_string;
			case type::i1:
				return false;
			case type::i8:
			case type::i16:
			case type::i32:
			case type::i64:
			case type::f32:
			case type::f64:
				return (number) 0;
			case type::any:
			case type::nil:
				return nil;
			case type::exc:
				return exception_marker;
			case type::bb:
			case type::none:
			case type::nfni:
			case type::vmopr:
			case type::vty:
			case type::dty:
			default:
				LI_ASSERT_MSG("invalid data type for any decaying.", false);
		}
	}

	// Load/Store from data types.
	//
	any  any::load_from(const void* data, li::type t) {
		if (t < type::obj)
			t = type::obj;
		switch (t) {
			case type::obj:
				return *(object* const*) data;
			case type::tbl:
				return *(table* const*) data;
			case type::arr:
				return *(array* const*) data;
			case type::fn:
				return *(function* const*) data;
			case type::str:
				return *(string* const*) data;
			case type::vcl:
				return *(vclass* const*) data;
			case type::i1:
				return *(const bool*) data;
			case type::i8:
				return (number) * (const int8_t*) data;
			case type::i16:
				return (number) * (const int16_t*) data;
			case type::i32:
				return (number) * (const int32_t*) data;
			case type::i64:
				return (number) * (const int64_t*) data;
			case type::f32:
				return (number) * (const float*) data;
			case type::f64:
				return (number) * (const double*) data;
			case type::any:
				return *(const any_t*) data;
			case type::nil:
			case type::none:
				return nil;
			case type::exc:
				return exception_marker;
			case type::bb:
			case type::nfni:
			case type::vmopr:
			case type::vty:
			case type::dty:
			default:
				LI_ASSERT_MSG("invalid data type for any decaying.", false);
		}
	}
	void any::store_at(void* data, li::type t) const {
		if (t < type::obj) {
			t = type::obj;
		}
		switch (t) {
			case type::obj:
			case type::tbl:
			case type::arr:
			case type::fn:
			case type::str:
			case type::vcl:
				LI_ASSERT(to_type(type()) == t);
				*(gc::header**) data = as_gc();
				break;
			case type::i1:
				LI_ASSERT(is_bool());
				*(bool*) data = as_bool();
				break;
			case type::i8:
				LI_ASSERT(is_num());
				*(int8_t*) data = int8_t(as_num());
				break;
			case type::i16:
				LI_ASSERT(is_num());
				*(int16_t*) data = int16_t(as_num());
				break;
			case type::i32:
				LI_ASSERT(is_num());
				*(int32_t*) data = int32_t(as_num());
				break;
			case type::i64:
				LI_ASSERT(is_num());
				*(int64_t*) data = int64_t(as_num());
				break;
			case type::f32:
				LI_ASSERT(is_num());
				*(float*) data = float(as_num());
				break;
			case type::f64:
				LI_ASSERT(is_num());
				*(double*) data = double(as_num());
				break;
			case type::any:
				*(any_t*) data = *this;
				break;
			case type::nil:
				LI_ASSERT(*this == nil);
				*(any_t*) data = *this;
				break;
			case type::exc:
				LI_ASSERT(*this == exception_marker);
				*(any_t*) data = *this;
				break;
			case type::none:
			case type::bb:
			case type::nfni:
			case type::vmopr:
			case type::vty:
			case type::dty:
			default:
				util::abort("invalid data type for any decaying.");
		}
	}
};