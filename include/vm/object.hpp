#pragma once
#include <bit>
#include <lib/std.hpp>
#include <memory>
#include <span>
#include <type_traits>
#include <util/typeinfo.hpp>
#include <vector>
#include <vm/function.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/traits.hpp>

namespace li {
	struct field_info {
		static constexpr uint32_t max_offset = (uint32_t{1} << 29) - 1;

		type     ty             = type::none;
		uint32_t offset : 29    = 0;  // If non static (base+offset = default value).
		uint32_t is_static : 1  = 0;  // 1=base=vclass, 0=base=object.
		uint32_t is_dyn : 1     = 0;
		uint32_t is_atomic : 1  = 0;
		uint64_t class_identity = 0;  // Immutable expected identity when ty is a user class.
	};
	struct field_pair {
		string*    key   = nullptr;
		field_info value = {};
	};

	enum class property_access : uint8_t {
		get,
		set,
	};
	struct property_definition {
		string*         name    = nullptr;
		function*       method  = nullptr;
		property_access access  = property_access::get;
		bool            dynamic = false;
	};

	// Reserves a process-global class identity before member functions are
	// compiled, allowing cycle-free own-class type guards.
	uint64_t reserve_class_identity(vm* L);

	// Tests an object against a process-global class identity, including bases.
	uint8_t LI_CC class_matches(any_t value, uint64_t identity);

	struct vclass : gc::node<vclass, type_class> {
		// Instantiates a new class type using a previously reserved identity.
		// Values in the input spans are borrowed.
		//
		static vclass* create(vm* L, string* name, std::span<const field_pair> fields, std::span<const uint8_t> default_values,
			 std::span<const uint8_t> static_values, vclass* super, uint64_t identity, bool value_semantics = false);

		// Replaces the constructor or instance initializer with a borrowed function.
		//
		bool set_ctor(vm* L, function* value);
		bool set_initializer(vm* L, function* value);
		bool set_constructor_body(vm* L, function* value);
		bool define_property(vm* L, const property_definition& value);

		// Type information.
		//
		vclass*                           super           = nullptr;  // Super class.
		util::type_id                     cxx_tid         = 0;        // If created by request of a native C++ module, the compile-time type identifier.
		uint64_t                          identity        = 0;        // Process-global immutable class identity, preserved by shared clones.
		int32_t                           vm_tid          = 0;        // VM type identifier.
		string*                           name            = nullptr;  // Type name.
		bool                              value_semantics = false;    // Instances compare and copy by field value.
		table*                            attributes      = nullptr;  // Immutable declaration metadata.
		std::shared_ptr<generic_template> generic;                    // Parser-owned generic declaration and instantiation cache.

		// Trait information.
		//
		trait_set*                       traits           = nullptr;
		function*                        ctor             = &lib::detail::builtin_null_function;
		function*                        initializer      = nullptr;  // Initializes an already allocated instance, including base state.
		function*                        constructor_body = nullptr;  // Explicit new! body; invoked by the native constructor dispatcher.
		uint64_t                         dynamic_traits   = 0;
		std::vector<property_definition> properties;

		// Field information.
		//
		msize_t object_length = 0;
		msize_t static_length = 0;
		msize_t num_fields    = 0;
		alignas(uint64_t) uint8_t static_data[];
		// uint8_t    padding[];
		// uint8_t    default_data[];
		// uint8_t    padding[];
		// field_pair field_array[]; // TODO: Sort for binary search?

		static constexpr size_t align_payload(size_t offset, size_t alignment) { return (offset + alignment - 1) & ~(alignment - 1); }
		static constexpr size_t default_offset(msize_t static_length) { return align_payload(static_length, alignof(any)); }
		static constexpr size_t fields_offset(msize_t static_length, msize_t object_length) {
			return align_payload(default_offset(static_length) + object_length, alignof(field_pair));
		}
		static constexpr size_t payload_size(msize_t static_length, msize_t object_length, msize_t num_fields) {
			return fields_offset(static_length, object_length) + sizeof(field_pair) * num_fields;
		}

		// Range getters.
		//
		uint8_t*              static_space() { return &static_data[0]; }
		uint8_t*              default_space() { return static_space() + default_offset(static_length); }
		std::span<field_pair> fields() { return {reinterpret_cast<field_pair*>(static_space() + fields_offset(static_length, object_length)), num_fields}; }
	};

	// Returns an owned constructor reference retained under the class guard.
	bool      object_equals(vm* L, object* lhs, object* rhs);
	function* pin_class_ctor(vm* L, vclass* owner);

	// Property metadata is searched from the most-derived class upward. The
	// returned function is owned; the predicate retains no descriptor values.
	function* pin_class_property(vm* L, vclass* owner, string* name, property_access access);
	bool      class_has_property(vclass* owner, string* name);

	struct object : gc::node<object, type_object /*<, maps to TID*/> {
		// Instantiates an object type.
		//
		static object* create(vm* L, vclass* c);

		// TODO:
		// template<typename T>
		// static object* create(vm* L, T value);
		// template<typename T>
		// static object* create(vm* L, T* ptr);
		// template<typename T>
		// static object* create(vm* L, std::unique_ptr<T> ptr);
		// template<typename T>
		// static object* create(vm* L, std::shared_ptr<T> ptr);

		// Type information.
		//
		vclass* cl = nullptr;

		// Data pointer.
		//
		uint8_t* data = nullptr;

		// GC hook.
		//
		void (*gc_hook)(object*) = nullptr;
		bool finalizable         = true;  // Failed explicit construction never runs del!.

		// Context.
		//
		alignas(uint64_t) uint8_t context[];

		// Copies the inline instance state into a fresh object owned by L.
		//
		object* copy(vm* L) const;

		// Duplicates the object.
		//
		object* duplicate(vm* L);

		// Get/Set, setter returns false if it threw an error.
		//
		any_t get(string* k) const;
		bool  set(vm* L, string* k, any_t v);

		// void*         self = nullptr;
		// util::type_id tid  = 0;
		// size_t        data[];
		//
		//// Type-check helper for any.
		////
		// template<typename T>
		// static T* get_if(any a) {
		//	if (a.is_obj()) {
		//		return a.as_obj()->get_if<T>();
		//	}
		//	return nullptr;
		// }
		//
		//// Creates a object by-value.
		////
		// template<typename T, typename... Tx>
		// static object* create(vm* L, Tx&&... args) {
		//	object* result = allocate(L, sizeof(T));
		//	result->self     = result->data;
		//	result->tid      = util::type_id_v<T>;
		//	new (result->data) T(std::forward<Tx>(args)...);
		//
		//	if constexpr (!std::is_trivially_destructible_v<std::remove_cvref_t<T>>) {
		//		result->set_trait(L, trait::gc, function::create(L, [](vm* L, any* args, slot_t n) {
		//			if (!args[1].is_udt()) [[unlikely]] {
		//				return L->error("gc type mismatch");
		//			}
		//			auto* udt = args[1].as_udt();
		//			if (!udt->is_no_cv<T>()) [[unlikely]] {
		//				return L->error("gc type mismatch");
		//			}
		//			std::destroy_at((std::remove_cvref_t<T>*) udt->self);
		//			udt->self = nullptr;
		//			udt->tid  = 0;
		//			return L->ok();
		//		}));
		//	}
		//	return result;
		// }
		//
		//// Creates a object by-pointer.
		////
		// template<typename T>
		// static object* create(vm* L, T* ptr, size_t extra_data = 0) {
		//	object* result = allocate(L, extra_data);
		//	result->self     = (void*) ptr;
		//	result->tid      = util::type_id_v<T>;
		//	return result;
		// }
		// template<typename T, typename Dx>
		// static object* create(vm* L, std::unique_ptr<T, Dx> ptr) {
		//	object* result = create(L, ptr.get(), std::is_empty_v<Dx> ? 0 : sizeof(Dx));
		//	if constexpr (!std::is_empty_v<Dx>)
		//		new (result->data) Dx(std::move(ptr.get_deleter()));
		//	result->set_trait(L, trait::gc, function::create(L, [](vm* L, any* args, slot_t n) {
		//		if (!args[1].is_udt()) [[unlikely]] {
		//			return L->error("gc type mismatch");
		//		}
		//		auto* udt = args[1].as_udt();
		//		if (!udt->is_no_cv<T>()) [[unlikely]] {
		//			return L->error("gc type mismatch");
		//		}
		//
		//		(*(Dx*) udt->data)((std::remove_cvref_t<T>*) udt->self);
		//		udt->self = nullptr;
		//		udt->tid  = 0;
		//		return L->ok();
		//	}));
		//	return result;
		// }
		// template<typename T>
		// static object* create(vm* L, std::shared_ptr<T> ptr) {
		//	object* result = create(L, ptr.get(), sizeof(std::shared_ptr<T>));
		//	new (result->data) std::shared_ptr<T>(std::move(ptr));
		//
		//	result->set_trait(L, trait::gc, function::create(L, [](vm* L, any* args, slot_t n) {
		//		if (!args[1].is_udt()) [[unlikely]] {
		//			return L->error("gc type mismatch");
		//		}
		//		auto* udt = args[1].as_udt();
		//		if (!udt->is_no_cv<T>()) [[unlikely]] {
		//			return L->error("gc type mismatch");
		//		}
		//		std::destroy_at((std::shared_ptr<T>*) udt->data);
		//		udt->self = nullptr;
		//		udt->tid  = 0;
		//		return L->ok();
		//	}));
		//	return result;
		// }
		//
		//// Getters.
		////
		// template<typename T>
		// T* get() const {
		//	return (T*) self;
		// }
		// template<typename T>
		// T* get_if() const {
		//	return is<T>() ? get<T>() : nullptr;
		// }
		// template<typename T>
		// bool is() const {
		//	return util::test_type_id<T>(tid);
		// }
		// template<typename T>
		// bool is_no_cv() const {
		//	return util::test_type_id_no_cv<T>(tid);
		// }
	};
};