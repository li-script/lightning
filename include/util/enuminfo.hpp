#pragma once
#include <array>
#include <string>
#include <string_view>
#include <util/common.hpp>
#include <util/typeinfo.hpp>
#include <utility>

namespace li::util {
#define LI_REPEAT_1(_, x)   _(x)
#define LI_REPEAT_8(_, x)   LI_REPEAT_1(_, x) LI_REPEAT_1(_, x + 1) LI_REPEAT_1(_, x + 2) LI_REPEAT_1(_, x + 3) LI_REPEAT_1(_, x + 4) LI_REPEAT_1(_, x + 5) LI_REPEAT_1(_, x + 6) LI_REPEAT_1(_, x + 7)
#define LI_REPEAT_64(_, x)  LI_REPEAT_8(_, x) LI_REPEAT_8(_, x + (8 * 1)) LI_REPEAT_8(_, x + (8 * 2)) LI_REPEAT_8(_, x + (8 * 3)) LI_REPEAT_8(_, x + (8 * 4)) LI_REPEAT_8(_, x + (8 * 5)) LI_REPEAT_8(_, x + (8 * 6)) LI_REPEAT_8(_, x + (8 * 7))

	// Used to generate names for enum types.
	//
	template<typename T>
	struct enum_namer {
		static constexpr int64_t iteration_limit = 64;
		using value_type                         = std::underlying_type_t<T>;

		// Generates the name for the given enum.
		//
		template<T Q>
		static constexpr std::string_view generate() {
			std::string_view name = const_tag<Q>::to_string();
			if (name[0] == '(' || uint8_t(name[0] - '0') <= 9 || name[0] == '-')
				return std::string_view{};
			size_t n = name.rfind(':');
			if (n != std::string::npos)
				name.remove_prefix(n + 1);
			return name;
		}
		inline static constexpr int64_t                                       min_value     = std::is_signed_v<value_type> ? (generate<T(-2)>().empty() ? -1 : -(iteration_limit / 2)) : 0;
		inline static constexpr std::array<std::string_view, iteration_limit> linear_series = {
#define GEN_ENUM(val) generate<T(val)>(),
			 LI_REPEAT_64(GEN_ENUM, min_value)
#undef GEN_ENUM
		};

		// String conversion.
		//
		static constexpr std::string_view name_enum(T v) {
			int64_t index = (value_type(v) - min_value);
			if (0 <= index && index < iteration_limit)
				return linear_series[index];
			else
				return {};
		}
	};

	template<typename T>
	inline constexpr std::string_view name_enum(T value) {
		return enum_namer<T>::name_enum(value);
	}
};