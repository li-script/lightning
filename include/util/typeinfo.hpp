#pragma once
#include <util/common.hpp>
#include <string_view>
#include <string>
#include <array>
#include <algorithm>

// Link-time type-id generation.
//
namespace li::util {
	// Type/value namers.
	//
	namespace detail {
		template<typename T>
		struct type_namer {
			template<typename __id__ = T>
			static _CONSTEVAL std::string_view _id__() {
				auto [sig, begin, delta, end] = std::tuple {
#if LI_GNU
					std::string_view{__PRETTY_FUNCTION__}, std::string_view{"_id__"}, +3, "]"
#else
					std::string_view{__FUNCSIG__}, std::string_view{"_id__"}, +1, ">"
#endif
				};

				// Find the beginning of the name.
				//
				size_t f = sig.size();
				while (sig.substr(--f, begin.size()).compare(begin) != 0)
					if (f == 0)
						return "";
				f += begin.size() + delta;

				// Find the end of the string.
				//
				auto l = sig.find_first_of(end, f);
				if (l == std::string::npos)
					return "";

				// Return the value.
				//
				auto r = sig.substr(f, l - f);
				if (r.size() > 7 && r.substr(0, 7) == "struct ") {
					r.remove_prefix(7);
				}
				if (r.size() > 6 && r.substr(0, 6) == "class ") {
					r.remove_prefix(6);
				}
				return r;
			}

			static constexpr auto name = []() {
				constexpr std::string_view          view = type_namer<T>::_id__<T>();
				std::array<char, view.length() + 1> data = {};
				std::copy(view.begin(), view.end(), data.data());
				return data;
			}();
			inline _CONSTEVAL operator std::string_view() const { return {&name[0], &name[name.size() - 1]}; }
			inline _CONSTEVAL operator const char*() const { return &name[0]; }
		};
		template<auto V>
		struct value_namer {
			template<auto __id__ = V>
			static _CONSTEVAL std::string_view _id__() {
				auto [sig, begin, delta, end] = std::tuple {
#if LI_GNU
					std::string_view{__PRETTY_FUNCTION__}, std::string_view{"_id__"}, +3, ']'
#else
					std::string_view{__FUNCSIG__}, std::string_view{"_id__"}, +0, '>'
#endif
				};

				// Find the beginning of the name.
				//
				size_t f = sig.rfind(begin);
				if (f == std::string::npos)
					return "";
				f += begin.size() + delta;

				// Find the end of the string.
				//
				auto l = sig.find(end, f);
				if (l == std::string::npos)
					return "";

				// Return the value.
				//
				return sig.substr(f, l - f);
			}

			static constexpr auto name = []() {
				constexpr std::string_view          view = value_namer<V>::_id__<V>();
				std::array<char, view.length() + 1> data = {};
				std::copy(view.begin(), view.end(), data.data());
				return data;
			}();
			inline _CONSTEVAL operator std::string_view() const { return {&name[0], &name[name.size() - 1]}; }
			inline _CONSTEVAL operator const char*() const { return &name[0]; }
		};

		static _CONSTEVAL uint32_t ctti_hash(const char* sig) {
			uint32_t tmp = 0x811c9dc5;
			while (*sig) {
				tmp ^= *sig++;
				tmp *= 0x01000193;
			}
			return tmp;
		}
	};

	// Type tag.
	//
	template<typename T>
	struct type_tag {
		using type = T;

		template<typename t = T>
		static _CONSTEVAL uint32_t hash() {
			return std::integral_constant<uint32_t, detail::ctti_hash(FUNCTION_NAME)>{};
		}
		template<auto C = 0>
		static _CONSTEVAL std::string_view to_string() {
			return detail::type_namer<T>{};
		}
		template<auto C = 0>
		static _CONSTEVAL const char* c_str() {
			return detail::type_namer<T>{};
		}
	};

	// Constant tag.
	//
	template<auto V>
	struct const_tag {
		using value_type                  = decltype(V);
		static constexpr value_type value = V;
		constexpr                   operator value_type() const noexcept { return value; }

		template<auto v = V>
		static _CONSTEVAL uint32_t hash() {
			return std::integral_constant<uint32_t, detail::ctti_hash(FUNCTION_NAME)>{};
		}
		template<auto C = 0>
		static _CONSTEVAL std::string_view to_string() {
			return detail::value_namer<V>{};
		}
		template<auto C = 0>
		static _CONSTEVAL const char* c_str() {
			return detail::value_namer<V>{};
		}
	};

	// String literal.
	//
	template<size_t N>
	struct string_literal {
		char value[N]{};
		constexpr string_literal(const char (&str)[N]) { std::copy_n(str, N, value); }

		// Observers.
		//
		inline constexpr const char*      c_str() const { return &value[0]; }
		inline constexpr const char*      data() const { return c_str(); }
		inline constexpr std::string_view view() const { return {c_str(), c_str() + size()}; }
		inline constexpr size_t           size() const { return N - 1; }
		inline constexpr size_t           length() const { return size(); }
		inline constexpr bool             empty() const { return size() == 0; }
		inline constexpr const char&      operator[](size_t n) const { return c_str()[n]; }
		inline constexpr auto             begin() const { return view().begin(); }
		inline constexpr auto             end() const { return view().end(); }

		// Decay to string view.
		//
		inline constexpr operator std::string_view() const { return view(); }
	};

	// Type IDs.
	//
	namespace detail {
		template<typename T>
		struct type_id
		{
			static constexpr uint32_t value = type_tag<T>::hash() << 1;
		};
		template<typename T>
		struct type_id<const T>
		{
			static constexpr uint32_t value = type_id<T>::value | 1;
		};
		template<>
		struct type_id<void>
		{
			static constexpr uint32_t value = 0;
		};
	};

	using type_id = uint32_t;
	template<typename T>
	static constexpr type_id type_id_v = detail::type_id<T>::value;

	LI_INLINE inline bool test_type_id(type_id a, type_id b) noexcept { return a == (b | (a & 1)); }  // For conversion to A.
	LI_INLINE inline bool test_type_id_no_cv(type_id a, type_id b) noexcept { return (a ^ b) <= 1; }

	template<typename T>
	LI_INLINE inline bool test_type_id_no_cv(type_id i) noexcept {
		return test_type_id_no_cv(type_id_v<T>, i);
	}
	template<typename T>
	LI_INLINE inline bool test_type_id(type_id i) noexcept {
		return test_type_id(type_id_v<T>, i);
	}
	LI_INLINE inline constexpr bool is_type_id_const(type_id i) noexcept { return i & 1; }
};