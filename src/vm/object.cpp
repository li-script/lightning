#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <vm/gc.hpp>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/shared.hpp>
#include <vm/table.hpp>

namespace li {
	// Internal type-set implementation.
	//
	struct type_set : gc::node<type_set> {
		static constexpr size_t min_size = 512;

		vclass* entries[];

		// Container interface.
		//
		size_t   size() { return this->object_bytes() / sizeof(vclass*); }
		vclass** begin() { return &entries[0]; }
		vclass** end() { return &entries[size()]; }

		// Resizing.
		//
		[[nodiscard]] type_set* nextsize(vm* L) {
			size_t n  = size();
			auto   ts = L->alloc<type_set>((n + (n >> 1)) * sizeof(vclass*));
			auto   it = std::copy(begin(), end(), ts->begin());
			std::fill(it, ts->end(), nullptr);
			return ts;
		}
	};

	void typeset_init(vm* L) {
		L->typeset = L->alloc<type_set>(type_set::min_size * sizeof(vclass*));
		range::fill(*L->typeset, nullptr);
	}
	vclass* typeset_fetch(vm* L, type id) {
		if (id >= type::obj)
			return nullptr;
		int32_t idx = -(int32_t(id) + 1);
		if (L->typeset->size() <= idx)
			return nullptr;
		return L->typeset->entries[idx];
	}
	static void typeset_push(vm* L, vclass* cl) {
		int32_t idx;
		if (auto it = range::find(*L->typeset, nullptr); it != L->typeset->end()) {
			idx = int32_t(it - L->typeset->begin());
		} else {
			idx           = int32_t(L->typeset->size());
			type_set* old = L->typeset;
			L->typeset    = old->nextsize(L);
			rc::release(L, old);
		}
		L->typeset->entries[idx] = cl;
		cl->vm_tid               = -(idx + 1);
	}

	uint64_t reserve_class_identity(vm* L) {
		static std::atomic<uint64_t> counter{1};
		const uint64_t               identity = counter.fetch_add(1, std::memory_order_relaxed);
		if (identity == 0 || identity == std::numeric_limits<uint64_t>::max())
			L->panic("class identity overflow");
		return identity;
	}

	uint8_t LI_CC class_matches(any_t value, uint64_t identity) {
		if (!value.is_obj() || !identity)
			return false;
		for (vclass* current = value.as_obj()->cl; current; current = current->super) {
			if (current->identity == identity)
				return true;
		}
		return false;
	}

	static bool field_holds_reference(type ty) { return ty == type::any || is_gc_data(ty); }
	static void retain_field(const void* data, type ty) {
		if (field_holds_reference(ty))
			rc::retain(any::load_from(data, ty));
	}
	static void release_field(vm* L, void* data, type ty) {
		if (!field_holds_reference(ty))
			return;

		any value = any::load_from(data, ty);
		if (ty == type::any) {
			any(nil).store_at(data, ty);
		} else {
			gc::header* cleared = nullptr;
			memcpy(data, &cleared, sizeof(cleared));
		}
		rc::release(L, value);
	}
	static bool replace_field(vm* L, void* data, type ty, any_t value) {
		any previous = any::load_from(data, ty);
		if (previous.value == value.value)
			return true;
		if (!rc::try_retain(L, value))
			return false;
		any(value).store_at(data, ty);
		rc::release(L, previous);
		return true;
	}

	template<typename T>
	static bool is_valid_integer(number value) {
		if (!std::isfinite(value) || value != std::trunc(value))
			return false;
		if constexpr (sizeof(T) == sizeof(int64_t)) {
			return value >= -0x1p63 && value < 0x1p63;
		} else {
			return value >= number(std::numeric_limits<T>::lowest()) && value <= number(std::numeric_limits<T>::max());
		}
	}
	static bool value_fits_field(any_t value, const field_info& field) {
		const type ty = field.ty;
		if (ty < type::obj)
			return class_matches(value, field.class_identity);

		switch (ty) {
			case type::obj:
				return value.is_obj();
			case type::tbl:
				return value.is_tbl();
			case type::arr:
				return value.is_arr();
			case type::fn:
				return value.is_fn();
			case type::str:
				return value.is_str();
			case type::vcl:
				return value.is_vcl();
			case type::weak:
				return value.is_weak();
			case type::tarr:
				return value.is_tarr();
			case type::i1:
				return value.is_bool();
			case type::i8:
				return value.is_num() && is_valid_integer<int8_t>(value.as_num());
			case type::i16:
				return value.is_num() && is_valid_integer<int16_t>(value.as_num());
			case type::i32:
				return value.is_num() && is_valid_integer<int32_t>(value.as_num());
			case type::i64:
				return value.is_num() && is_valid_integer<int64_t>(value.as_num());
			case type::f32:
			case type::f64:
				return value.is_num();
			case type::any:
				return true;
			case type::nil:
				return value == nil;
			case type::exc:
				return value == exception_marker;
			default:
				return false;
		}
	}

	// Destruction hooks and maintenance of the weak type registry.
	//
	void gc::destroy(vm* L, object* o) {
		vclass* cl = o->cl;

		auto hook  = o->gc_hook;
		o->gc_hook = nullptr;
		if (hook)
			hook(o);

		if (cl && o->data) {
			for (auto& field : cl->fields()) {
				if (!field.value.is_static)
					release_field(L, &o->data[field.value.offset], field.value.ty);
			}
		}

		o->data = nullptr;
		o->cl   = nullptr;
		rc::release(L, cl);
	}
	void gc::destroy(vm* L, vclass* o) {
		if (o->vm_tid < 0 && L->typeset) {
			size_t idx = size_t(-(o->vm_tid + 1));
			if (idx < L->typeset->size()) {
				auto& entry = L->typeset->entries[idx];
				LI_ASSERT(entry == o);
				entry = nullptr;
			}
		}

		destroy_trait_set(L, o->traits);
		for (auto& field : o->fields()) {
			void* data =
				 field.value.is_static ? static_cast<void*>(&o->static_space()[field.value.offset]) : static_cast<void*>(&o->default_space()[field.value.offset]);
			release_field(L, data, field.value.ty);

			string* key = field.key;
			field.key   = nullptr;
			rc::release(L, key);
		}

		for (property_definition& property : o->properties) {
			string*   property_name   = property.name;
			function* property_method = property.method;
			property.name             = nullptr;
			property.method           = nullptr;
			rc::release(L, property_name);
			rc::release(L, property_method);
		}
		std::destroy_at(&o->properties);

		function* ctor             = o->ctor;
		function* initializer      = o->initializer;
		function* constructor_body = o->constructor_body;
		string*   name             = o->name;
		vclass*   super            = o->super;
		table*    attributes       = o->attributes;
		o->ctor                    = nullptr;
		o->initializer             = nullptr;
		o->constructor_body        = nullptr;
		o->name                    = nullptr;
		o->super                   = nullptr;
		o->attributes              = nullptr;
		o->generic.reset();
		rc::release(L, ctor);
		rc::release(L, initializer);
		rc::release(L, constructor_body);
		rc::release(L, name);
		rc::release(L, super);
		rc::release(L, attributes);
	}

	namespace {
		struct equality_frame {
			object*               lhs;
			object*               rhs;
			const equality_frame* parent;
		};

		bool object_equals_impl(vm* L, object* lhs, object* rhs, const equality_frame* parent) {
			(void) L;
			if (!lhs || !rhs || !lhs->cl || !rhs->cl || !lhs->cl->value_semantics || !rhs->cl->value_semantics || lhs->cl->identity != rhs->cl->identity)
				return false;
			for (const equality_frame* frame = parent; frame; frame = frame->parent) {
				if (frame->lhs == lhs && frame->rhs == rhs)
					return true;
			}
			const equality_frame current{lhs, rhs, parent};
			for (const field_pair& field : lhs->cl->fields()) {
				if (field.value.is_static)
					continue;
				any left  = any::load_from(lhs->data + field.value.offset, field.value.ty);
				any right = any::load_from(rhs->data + field.value.offset, field.value.ty);
				if (left.is_obj() && right.is_obj() && left.as_obj()->cl && right.as_obj()->cl && left.as_obj()->cl->value_semantics &&
					 right.as_obj()->cl->value_semantics && left.as_obj()->cl->identity == right.as_obj()->cl->identity) {
					if (!object_equals_impl(L, left.as_obj(), right.as_obj(), &current))
						return false;
				} else if (!left.equals(right)) {
					return false;
				}
			}
			return true;
		}
	}

	bool object_equals(vm* L, object* lhs, object* rhs) { return object_equals_impl(L, lhs, rhs, nullptr); }

	object* object::copy(vm* L) const {
		object* result      = L->alloc<object>(cl->object_length);
		result->cl          = cl;
		result->data        = result->context;
		result->gc_hook     = nullptr;
		result->finalizable = true;
		result->type_id     = cl->vm_tid;
		rc::retain(cl);
		if (cl->object_length)
			std::memcpy(result->data, data, cl->object_length);
		for (const field_pair& field : cl->fields()) {
			if (!field.value.is_static)
				retain_field(result->data + field.value.offset, field.value.ty);
		}
		return result;
	}

	// Duplicates the object.
	//
	object* object::duplicate(vm* L) {
		if (!shared) {
			if (data != context || gc_hook) {
				if (!rc::try_retain(L, this))
					return nullptr;
				return this;
			}

			object* result = L->duplicate(this);
			result->data   = result->context;
			rc::retain(result->cl);
			for (auto& field : result->cl->fields()) {
				if (!field.value.is_static)
					retain_field(&result->data[field.value.offset], field.value.ty);
			}
			result->type_id = result->cl->vm_tid;
			return result;
		}

		shared::recursive_guard guard(L, this);
		if (data != context || gc_hook) {
			if (!rc::try_retain(L, this))
				return nullptr;
			return this;
		}

		constexpr size_t fixed_payload = sizeof(object) - sizeof(gc::header);
		const size_t     object_length = object_bytes();
		if (object_length < fixed_payload)
			L->panic("object duplication size overflow");
		object* result = shared::allocate<object>(L, object_length - fixed_payload);
		std::memcpy(static_cast<void*>(std::next(static_cast<gc::header*>(result))), static_cast<const void*>(std::next(static_cast<const gc::header*>(this))),
			 object_length);
		result->data = result->context;
		rc::retain(result->cl);
		for (auto& field : result->cl->fields()) {
			if (!field.value.is_static)
				retain_field(&result->data[field.value.offset], field.value.ty);
		}
		result->type_id = result->cl->vm_tid;
		return result;
	}

	// Instantiates an object type.
	//
	object* object::create(vm* L, vclass* cl) {
		object* result = L->alloc<object>(cl->object_length);
		result->cl     = cl;
		result->data   = result->context;
		rc::retain(cl);
		if (cl->object_length)
			memcpy(result->context, cl->default_space(), cl->object_length);
		for (auto& field : cl->fields()) {
			if (!field.value.is_static)
				retain_field(&result->data[field.value.offset], field.value.ty);
		}
		result->type_id = cl->vm_tid;
		return result;
	}

	// Instantiates a new class type.
	//
	vclass* vclass::create(vm* L, string* name, std::span<const field_pair> fields, std::span<const uint8_t> default_values,
		 std::span<const uint8_t> static_values, vclass* super, uint64_t identity, bool value_semantics) {
		if (super && (value_semantics || super->value_semantics))
			L->panic("value-semantics classes cannot participate in inheritance");
		msize_t object_length = super ? super->object_length : 0;
		msize_t static_length = super ? super->static_length : 0;
		for (auto& field : fields) {
			msize_t end = field.value.offset + size_of_data(field.value.ty);
			if (field.value.is_static)
				static_length = std::max(end, static_length);
			else
				object_length = std::max(end, object_length);
		}
		if (default_values.size() != object_length || static_values.size() != static_length)
			util::abort("invalid class field layout");
		if (!identity || identity == std::numeric_limits<uint64_t>::max())
			util::abort("invalid reserved class identity");

		vclass* result          = L->alloc<vclass>(payload_size(static_length, object_length, (msize_t) fields.size()));
		result->cxx_tid         = util::type_id_v<void>;
		result->super           = super;
		result->identity        = identity;
		result->name            = name;
		result->value_semantics = value_semantics;
		result->attributes      = nullptr;
		result->object_length   = object_length;
		result->static_length   = static_length;
		result->num_fields      = (msize_t) fields.size();

		memset(result->static_space(), 0, payload_size(static_length, object_length, result->num_fields));
		if (!static_values.empty())
			memcpy(result->static_space(), static_values.data(), static_values.size());
		if (!default_values.empty())
			memcpy(result->default_space(), default_values.data(), default_values.size());
		if (!fields.empty())
			memcpy(result->fields().data(), fields.data(), fields.size_bytes());
		for (field_pair& field : result->fields()) {
			if (field.value.ty >= type::obj)
				continue;
			vclass* expected = typeset_fetch(L, field.value.ty);
			if (!expected)
				util::abort("invalid class field type");
			field.value.class_identity = expected->identity;
		}

		rc::retain(result->super);
		rc::retain(result->name);
		rc::retain(result->ctor);
		for (auto& field : result->fields()) {
			rc::retain(field.key);
			const void* data = field.value.is_static ? static_cast<const void*>(&result->static_space()[field.value.offset])
																  : static_cast<const void*>(&result->default_space()[field.value.offset]);
			retain_field(data, field.value.ty);
		}

		typeset_push(L, result);
		return result;
	}

	static bool set_class_function(vm* L, vclass* owner, function*& slot, function* value) {
		if (!owner->shared) {
			if (slot == value)
				return true;
			if (!rc::try_retain(L, value))
				return false;
			function* previous = slot;
			slot               = value;
			rc::release(L, previous);
			return true;
		}

		shared::prepared_value prepared = shared::prepare_store(L, owner, value ? any(value) : any(nil));
		if (!prepared.ok || (prepared.value != nil && !prepared.value.is_fn())) {
			shared::finish_store(L, prepared);
			return false;
		}
		shared::recursive_guard guard(L, owner);
		function*               stored = prepared.value == nil ? nullptr : prepared.value.as_fn();
		if (slot == stored) {
			shared::finish_store(L, prepared);
			return true;
		}
		if (!rc::try_retain(L, stored)) {
			shared::finish_store(L, prepared);
			return false;
		}
		function* previous = slot;
		slot               = stored;
		rc::release(L, previous);
		shared::finish_store(L, prepared);
		return true;
	}
	bool vclass::set_ctor(vm* L, function* value) { return set_class_function(L, this, ctor, value); }
	bool vclass::set_initializer(vm* L, function* value) { return set_class_function(L, this, initializer, value); }
	bool vclass::set_constructor_body(vm* L, function* value) { return set_class_function(L, this, constructor_body, value); }

	namespace {
		const property_definition* find_direct_property(const vclass* owner, string* name, property_access access) {
			for (const property_definition& property : owner->properties) {
				if (property.access == access && string_value_equals(property.name, name))
					return &property;
			}
			return nullptr;
		}

		bool validate_property_override(vm* L, vclass* owner, string* name, property_access access) {
			bool duplicate = false;
			{
				shared::recursive_guard guard(L, owner);
				duplicate = find_direct_property(owner, name, access) != nullptr;
			}
			if (duplicate) {
				L->error("duplicate property accessor.");
				return false;
			}
			bool final_override = false;
			for (vclass* current = owner->super; current; current = current->super) {
				shared::recursive_guard guard(L, current);
				if (const property_definition* inherited = find_direct_property(current, name, access)) {
					final_override = !inherited->dynamic;
					break;
				}
			}
			if (final_override) {
				L->error("overriding final property accessor.");
				return false;
			}
			return true;
		}

		struct pinned_construction {
			function* initializer = nullptr;
			function* body        = nullptr;
		};

		pinned_construction pin_construction(vm* L, vclass* owner) {
			shared::recursive_guard guard(L, owner);
			pinned_construction     result{owner->initializer, owner->constructor_body};
			if (!rc::try_retain(L, result.initializer))
				return {};
			if (!rc::try_retain(L, result.body)) {
				rc::release(L, result.initializer);
				return {};
			}
			return result;
		}

		any_t LI_CC dispatch_class_constructor(vm* L, any* args, slot_t n_args) {
			if (!args[1].is_vcl())
				return L->error("class constructor invoked without class self");

			vclass*             owner = args[1].as_vcl();
			pinned_construction call  = pin_construction(L, owner);
			if (!call.body)
				return L->error("class constructor body unavailable");

			object* instance      = object::create(L, owner);
			instance->finalizable = false;

			if (call.initializer) {
				any initialized = L->call(0, any(call.initializer), any(instance));
				rc::release(L, call.initializer);
				call.initializer = nullptr;
				if (initialized.is_exc()) {
					rc::release(L, call.body);
					rc::release(L, instance);
					return exception_marker;
				}
				rc::release(L, initialized);
			}

			if (!L->vm_stack_headroom_available(size_t(n_args))) {
				rc::release(L, call.body);
				rc::release(L, instance);
				return L->vm_stack_exhausted();
			}
			for (slot_t index = n_args; index != 0; --index)
				L->push_stack(args[-(index - 1)]);
			any body_result = L->call(n_args, any(call.body), any(instance));
			rc::release(L, call.body);
			if (body_result.is_exc()) {
				rc::release(L, instance);
				return exception_marker;
			}
			rc::release(L, body_result);
			instance->finalizable = true;
			return any(instance);
		}

		struct constructor_dispatcher final : function {
			constructor_dispatcher() {
				gc::make_non_gc(this);
				invoke = &dispatch_class_constructor;
			}
		};

		function* explicit_constructor_dispatcher() {
			static constructor_dispatcher value;
			return &value;
		}
	}

	bool vclass::define_property(vm* L, const property_definition& value) {
		if (!value.name || !value.method) {
			L->error("property accessors require a name and function.");
			return false;
		}
		if (!validate_property_override(L, this, value.name, value.access))
			return false;

		shared::prepared_value prepared_name   = shared::prepare_store(L, this, any(value.name));
		shared::prepared_value prepared_method = prepared_name.ok ? shared::prepare_store(L, this, any(value.method)) : shared::prepared_value{};
		if (!prepared_name.ok || !prepared_method.ok || !prepared_name.value.is_str() || !prepared_method.value.is_fn()) {
			shared::finish_store(L, prepared_method);
			shared::finish_store(L, prepared_name);
			return false;
		}

		property_definition stored{
			 .name    = prepared_name.value.as_str(),
			 .method  = prepared_method.value.as_fn(),
			 .access  = value.access,
			 .dynamic = value.dynamic,
		};
		bool duplicate       = false;
		bool retained_name   = false;
		bool retained_method = false;
		{
			shared::recursive_guard guard(L, this);
			duplicate = find_direct_property(this, stored.name, stored.access) != nullptr;
			if (!duplicate) {
				retained_name = rc::try_retain(L, stored.name);
				if (retained_name)
					retained_method = rc::try_retain(L, stored.method);
				if (retained_method)
					properties.push_back(stored);
			}
		}
		if (retained_name && !retained_method)
			rc::release(L, stored.name);
		shared::finish_store(L, prepared_method);
		shared::finish_store(L, prepared_name);
		if (duplicate)
			L->error("duplicate property accessor.");
		return retained_method;
	}

	function* pin_class_ctor(vm* L, vclass* owner) {
		shared::recursive_guard guard(L, owner);
		function*               result = owner->constructor_body ? explicit_constructor_dispatcher() : owner->ctor;
		return rc::try_retain(L, result) ? result : nullptr;
	}

	function* pin_class_property(vm* L, vclass* owner, string* name, property_access access) {
		for (vclass* current = owner; current; current = current->super) {
			shared::recursive_guard guard(L, current);
			if (const property_definition* property = find_direct_property(current, name, access))
				return rc::try_retain(L, property->method) ? property->method : nullptr;
		}
		return nullptr;
	}

	bool class_has_property(vclass* owner, string* name) {
		for (vclass* current = owner; current; current = current->super) {
			shared::recursive_guard guard(nullptr, current);
			for (const property_definition& property : current->properties) {
				if (string_value_equals(property.name, name))
					return true;
			}
		}
		return false;
	}

	// Get/Set, setter returns false if it threw an error.
	//
	any_t object::get(string* k) const {
		for (auto& [kx, v] : cl->fields()) {
			if (string_value_equals(k, kx)) {
				const gc::header* owner = v.is_static ? static_cast<const gc::header*>(cl) : static_cast<const gc::header*>(this);
				const auto        load  = [&]() -> any_t {
					const void* p = v.is_static ? static_cast<const void*>(&cl->static_space()[v.offset]) : static_cast<const void*>(&data[v.offset]);
					return any::load_from(p, v.ty);
				};
				if (!owner->shared)
					return load();
				shared::recursive_guard guard(nullptr, const_cast<gc::header*>(owner));
				return load();
			}
		}
		return nil;
	}
	bool object::set(vm* L, string* k, any_t vv) {
		for (auto& [kx, v] : cl->fields()) {
			if (!string_value_equals(k, kx))
				continue;

			gc::header* owner;
			void*       p;
			if (v.is_static) {
				if (!v.is_dyn) [[unlikely]] {
					L->error("modifying constant field.");
					return false;
				}
				owner = cl;
				p     = &cl->static_space()[v.offset];
			} else {
				owner = this;
				p     = &data[v.offset];
			}

			if (owner->shared && v.is_atomic)
				return !shared::atomic_field_store(L, any(this), any(k), vv).is_exc();

			if (!owner->shared) {
				if (!value_fits_field(vv, v)) {
					L->error("invalid value for typed field.");
					return false;
				}
				return replace_field(L, p, v.ty, vv);
			}

			shared::prepared_value prepared = shared::prepare_store(L, owner, vv);
			if (!prepared.ok)
				return false;
			if (!value_fits_field(prepared.value, v)) {
				shared::finish_store(L, prepared);
				L->error("invalid value for typed field.");
				return false;
			}
			shared::recursive_guard guard(L, owner);
			const bool              replaced = replace_field(L, p, v.ty, prepared.value);
			shared::finish_store(L, prepared);
			return replaced;
		}
		L->error("field does not exist.");
		return false;
	}
};