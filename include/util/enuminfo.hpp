#pragma once
#include <util/common.hpp>
#include <util/typeinfo.hpp>
#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace li::util {
	namespace detail {
		template<typename Ti, typename T, Ti... I>
		inline constexpr auto make_constant_series(T&& f, [[maybe_unused]] std::integer_sequence<Ti, I...> seq)
		{
			using R = decltype( f( const_tag<( Ti ) 0>{} ) );
#if LI_MSVC
			// Needs to be default initializable on MSVC, fuck this.
			std::array<R, sizeof...( I )> arr = {};
			auto assign = [ & ] <Ti X> ( const_tag<X> tag, auto&& self )
			{
            arr[X] = f(tag);
            if constexpr ((X + 1) != (sizeof...(I)))
					self( const_tag<X + 1>{}, self );
			};
			assign( const_tag<Ti( 0 )>{}, assign );
			return arr;
#else
			return std::array<R, sizeof...( I )>{ f( const_tag<I>{} )... };
#endif
		}
		template<auto N, typename T>
		inline constexpr auto make_constant_series( T&& f )
		{
			return make_constant_series<decltype( N )>( std::forward<T>( f ), std::make_integer_sequence<decltype( N ), N>{} );
		}
	};

	// Used to generate names for enum types.
	//
	template<typename T>
	struct enum_namer {
#ifdef __INTELLISENSE__
		static constexpr int64_t iteration_limit = 1;
#else
		static constexpr int64_t iteration_limit = 256;
#endif
		using value_type                = std::underlying_type_t<T>;

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
		static constexpr int64_t                                       min_value     = std::is_signed_v<value_type> ? (generate<T(-2)>().empty() ? -1 : -(iteration_limit / 2)) : 0;
		static constexpr std::array<std::string_view, iteration_limit> linear_series = detail::make_constant_series<iteration_limit>([]<auto V>(const_tag<V>) { return generate<T(V + min_value)>(); });

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
	static constexpr std::string_view name_enum(T value) {
		return enum_namer<T>::name_enum(value);
	}
};