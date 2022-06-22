#pragma once
#include <bit>
#include <memory>
#include <span>
#include <type_traits>
#include <util/typeinfo.hpp>
#include <vm/function.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <lib/std.hpp>

namespace li {	
	struct field_info {
		type     ty            = type::none;
		uint32_t offset : 30   = 0;  // If non static (base+offset = default value).
		uint32_t is_static : 1 = 0;  // 1=base=vclass, 0=base=object.
		uint32_t is_dyn : 1    = 0;
	};
	struct field_pair {
		string*    key   = nullptr;
		field_info value = {};
	};

	struct vclass : gc::node<vclass, type_class> {
		// Instantiates a new class type.
		//
		static vclass* create(vm* L, string* name, std::span<const field_pair> fields);

		// Type information.
		//
		vclass*       super   = nullptr;  // Super class.
		util::type_id cxx_tid = 0;        // If created by request of a native C++ module, the compile-time type identifier.
		int32_t       vm_tid  = 0;        // VM type identifier.
		string*       name    = nullptr;  // Type name.

		// Trait information.
		//
		function* ctor = &lib::detail::builtin_null_function;

		// Field information.
		//
		msize_t object_length = 0;
		msize_t static_length = 0;
		msize_t num_fields    = 0;
		alignas(uint64_t) uint8_t static_data[];
		// uint8_t    default_data[];
		// field_pair field_array[]; // TODO: Sort for binary search?

		// Range getters.
		//
		uint8_t*              static_space() { return &static_data[0]; }
		uint8_t*              default_space() { return static_length + static_space(); }
		std::span<field_pair> fields() { return {(field_pair*) (default_space() + object_length), num_fields}; }
	};

	struct object : gc::node<object, type_object /*<, maps to TID*/> {
		// Instantiates an object type.
		//
		static object* create(vm* L, vclass* c);

		// TODO:
		//template<typename T>
		//static object* create(vm* L, T value);
		//template<typename T>
		//static object* create(vm* L, T* ptr);
		//template<typename T>
		//static object* create(vm* L, std::unique_ptr<T> ptr);
		//template<typename T>
		//static object* create(vm* L, std::shared_ptr<T> ptr);

		// Type information.
		//
		vclass* cl = nullptr;

		// Data pointer.
		//
		uint8_t* data = nullptr;

		// GC hook.
		//
		void (*gc_hook)(object*) = nullptr;

		// Context.
		//
		uint8_t context[];

		// Duplicates the object.
		//
		object* duplicate(vm* L);

		// Get/Set, setter returns false if it threw an error.
		//
		any_t get(string* k) const;
		bool  set(vm* L, string* k, any_t v);

		//void*         self = nullptr;
		//util::type_id tid  = 0;
		//size_t        data[];
		//
		//// Type-check helper for any.
		////
		//template<typename T>
		//static T* get_if(any a) {
		//	if (a.is_obj()) {
		//		return a.as_obj()->get_if<T>();
		//	}
		//	return nullptr;
		//}
		//
		//// Creates a object by-value.
		////
		//template<typename T, typename... Tx>
		//static object* create(vm* L, Tx&&... args) {
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
		//}
		//
		//// Creates a object by-pointer.
		////
		//template<typename T>
		//static object* create(vm* L, T* ptr, size_t extra_data = 0) {
		//	object* result = allocate(L, extra_data);
		//	result->self     = (void*) ptr;
		//	result->tid      = util::type_id_v<T>;
		//	return result;
		//}
		//template<typename T, typename Dx>
		//static object* create(vm* L, std::unique_ptr<T, Dx> ptr) {
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
		//}
		//template<typename T>
		//static object* create(vm* L, std::shared_ptr<T> ptr) {
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
		//}
		//
		//// Getters.
		////
		//template<typename T>
		//T* get() const {
		//	return (T*) self;
		//}
		//template<typename T>
		//T* get_if() const {
		//	return is<T>() ? get<T>() : nullptr;
		//}
		//template<typename T>
		//bool is() const {
		//	return util::test_type_id<T>(tid);
		//}
		//template<typename T>
		//bool is_no_cv() const {
		//	return util::test_type_id_no_cv<T>(tid);
		//}
	};
};