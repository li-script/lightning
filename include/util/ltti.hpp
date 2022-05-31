#pragma once
#include <util/common.hpp>

// Link-time type-id generation.
//
namespace li::util {
	namespace detail {
		template<typename T>
		alignas(2) inline char tag = 0;
	};

	template<typename T>
	LI_INLINE inline uint32_t get_type_id() noexcept {
		if constexpr (std::is_void_v<T>) {
			return 0;
		} else if constexpr (std::is_const_v<T>) {
			return get_type_id<std::remove_const_t<T>>() | 1;
		} else {
			return (uint32_t) (uintptr_t) &detail::tag<std::remove_cv_t<T>>;
		}
	}
	template<typename T>
	LI_INLINE inline bool check_type_id_no_cv(uint32_t i) noexcept {
		return (get_type_id<T>() ^ i) <= 1;
	}
	template<typename T>
	LI_INLINE inline bool check_type_id(uint32_t i) noexcept {
		return get_type_id<T>() == (i | std::is_const_v<T>);
	}
	LI_INLINE inline constexpr bool is_type_id_const(uint32_t i) noexcept { return i & 1; }
};