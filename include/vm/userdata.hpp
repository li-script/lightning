#pragma once
#include <bit>
#include <memory>
#include <span>
#include <type_traits>
#include <util/typeinfo.hpp>
#include <vm/function.hpp>
#include <vm/state.hpp>
#include <vm/traits.hpp>

namespace li {
	struct userdata : traitful_node<userdata, type_userdata> {
		static userdata* allocate(vm* L, size_t n) { return L->alloc<userdata>(n); }

		void*         self = nullptr;
		util::type_id tid  = 0;
		size_t        data[];

		// Type-check helper for any.
		//
		template<typename T>
		static T* get_if(any a) {
			if (a.is_udt()) {
				return a.as_udt()->get_if<T>();
			}
			return nullptr;
		}

		// Creates a userdata by-value.
		//
		template<typename T, typename... Tx>
		static userdata* create(vm* L, Tx&&... args) {
			userdata* result = allocate(L, sizeof(T));
			result->self     = result->data;
			result->tid      = util::type_id_v<T>;
			new (result->data) T(std::forward<Tx>(args)...);

			if constexpr (!std::is_trivially_destructible_v<std::remove_cvref_t<T>>) {
				result->set_trait(L, trait::gc, nfunction::create(L, [](vm* L, any* args, slot_t n) {
					if (!args[1].is_udt()) [[unlikely]] {
						return L->error("gc type mismatch");
					}
					auto* udt = args[1].as_udt();
					if (!udt->is_no_cv<T>()) [[unlikely]] {
						return L->error("gc type mismatch");
					}
					std::destroy_at((std::remove_cvref_t<T>*) udt->self);
					udt->self = nullptr;
					udt->tid  = 0;
					return true;
				}));
			}
			return result;
		}

		// Creates a userdata by-pointer.
		//
		template<typename T>
		static userdata* create(vm* L, T* ptr, size_t extra_data = 0) {
			userdata* result = allocate(L, extra_data);
			result->self     = (void*) ptr;
			result->tid      = util::type_id_v<T>;
			return result;
		}
		template<typename T, typename Dx>
		static userdata* create(vm* L, std::unique_ptr<T, Dx> ptr) {
			userdata* result = create(L, ptr.get(), std::is_empty_v<Dx> ? 0 : sizeof(Dx));
			if constexpr (!std::is_empty_v<Dx>)
				new (result->data) Dx(std::move(ptr.get_deleter()));
			result->set_trait(L, trait::gc, nfunction::create(L, [](vm* L, any* args, slot_t n) {
				if (!args[1].is_udt()) [[unlikely]] {
					return L->error("gc type mismatch");
				}
				auto* udt = args[1].as_udt();
				if (!udt->is_no_cv<T>()) [[unlikely]] {
					return L->error("gc type mismatch");
				}

				(*(Dx*) udt->data)((std::remove_cvref_t<T>*) udt->self);
				udt->self = nullptr;
				udt->tid  = 0;
				return true;
			}));
			return result;
		}
		template<typename T>
		static userdata* create(vm* L, std::shared_ptr<T> ptr) {
			userdata* result = create(L, ptr.get(), sizeof(std::shared_ptr<T>));
			new (result->data) std::shared_ptr<T>(std::move(ptr));

			result->set_trait(L, trait::gc, nfunction::create(L, [](vm* L, any* args, slot_t n) {
				if (!args[1].is_udt()) [[unlikely]] {
					return L->error("gc type mismatch");
				}
				auto* udt = args[1].as_udt();
				if (!udt->is_no_cv<T>()) [[unlikely]] {
					return L->error("gc type mismatch");
				}
				std::destroy_at((std::shared_ptr<T>*) udt->data);
				udt->self = nullptr;
				udt->tid  = 0;
				return true;
			}));
			return result;
		}

		// Getters.
		//
		template<typename T>
		T* get() const {
			return (T*) self;
		}
		template<typename T>
		T* get_if() const {
			return is<T>() ? get<T>() : nullptr;
		}
		template<typename T>
		bool is() const {
			return util::test_type_id<T>(tid);
		}
		template<typename T>
		bool is_no_cv() const {
			return util::test_type_id_no_cv<T>(tid);
		}
	};
};