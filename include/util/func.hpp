#pragma once
#include <util/typeinfo.hpp>
#include <tuple>

namespace li::util {
	// Invocation traits.
	//
	template<typename T, typename... Args>
	concept InvocableWith = requires(T&& x) { x(std::declval<Args>()...); };
	namespace detail {
		template<typename T, typename Ret, typename... Args>
		struct invoke_traits {
			static constexpr bool success = false;
		};
		template<typename T, typename Ret, typename... Args>
			requires InvocableWith<T, Args...>
		struct invoke_traits<T, Ret, Args...> {
			using R                       = decltype(std::declval<T&&>()(std::declval<Args&&>()...));
			static constexpr bool success = std::is_void_v<Ret> ? std::is_void_v<R> : std::is_convertible_v<R, Ret>;
		};
	};
	template<typename T, typename Ret, typename... Args>
	concept Invocable = detail::invoke_traits<T, Ret, Args...>::success;

	// Function traits.
	//
	template<typename F>
	struct function_traits {
		static constexpr bool is_valid  = false;
		static constexpr bool is_vararg = false;
		static constexpr bool is_lambda = false;
	};

	// Function pointers:
	//
	template<typename R, typename... Tx>
	struct function_traits<R (*)(Tx...)> {
		static constexpr bool is_valid  = true;
		static constexpr bool is_vararg = false;
		static constexpr bool is_lambda = false;

		using return_type = R;
		using arguments   = std::tuple<Tx...>;
		using owner       = void;
		using normal_form = R(Tx...);
	};
	template<typename R, typename... Tx>
	struct function_traits<R (*)(Tx..., ...)> : function_traits<R (*)(Tx...)> {
		static constexpr bool is_vararg = true;
	};

	// Member functions:
	//
	template<typename C, typename R, typename... Tx>
	struct function_traits<R (C::*)(Tx...)> {
		static constexpr bool is_valid  = true;
		static constexpr bool is_vararg = false;
		static constexpr bool is_lambda = false;

		using return_type = R;
		using arguments   = std::tuple<Tx...>;
		using owner       = C;
		using normal_form = R(Tx...);
	};
	template<typename C, typename R, typename... Tx>
	struct function_traits<R (C::*)(Tx..., ...)> : function_traits<R (C::*)(Tx...)> {
		static constexpr bool is_vararg = true;
	};
	template<typename C, typename R, typename... Tx>
	struct function_traits<R (C::*)(Tx...) const> {
		static constexpr bool is_valid  = true;
		static constexpr bool is_vararg = false;
		static constexpr bool is_lambda = false;

		using return_type = R;
		using arguments   = std::tuple<Tx...>;
		using owner       = const C;
		using normal_form = R(Tx...);
	};
	template<typename C, typename R, typename... Tx>
	struct function_traits<R (C::*)(Tx..., ...) const> : function_traits<R (C::*)(Tx...) const> {
		static constexpr bool is_vararg = true;
	};

	// Lambdas or callables.
	//
	template<typename F>
	concept CallableObject = requires { &F::operator(); };
	template<CallableObject F>
	struct function_traits<F> : function_traits<decltype(&F::operator())> {
		static constexpr bool is_lambda = true;
	};

	// Declares a light-weight std::function replacement.
	// - Note: will not copy the lambda objects, lifetime is left to the user to be managed.
	//
	template<typename F>
	struct function_view;
	template<typename Ret, typename... Args>
	struct function_view<Ret(Args...)> {
		// Hold object pointer, invocation wrapper and whether it is const qualified or not.
		//
		void* obj                 = nullptr;
		Ret (*fn)(void*, Args...) = nullptr;

		// Null construction.
		//
		constexpr function_view() {}
		constexpr function_view(std::nullptr_t) {}

		// Construct from any functor.
		//
		template<typename F>
			requires(Invocable<F, Ret, Args...> && !std::is_same_v<std::decay_t<F>, function_view>)
		constexpr function_view(F&& functor) {
			using Fn     = std::decay_t<F>;
			using Traits = function_traits<Fn>;

			// Lambda?
			//
			if constexpr (Traits::is_lambda) {
				// Stateless lambda?
				//
				if constexpr (std::is_empty_v<Fn>) {
					fn = [](void*, Args... args) -> Ret { return Fn{}(std::move(args)...); };
				}
				// Stateful lambda.
				//
				else {
					obj = (void*) &functor;
					fn  = [](void* obj, Args... args) -> Ret { return (*(Fn*) obj)(std::move(args)...); };
				}
			}
			// Function pointer?
			//
			else {
				obj = (void*) functor;
				fn  = [](void* obj, Args... args) -> Ret { return ((Fn) obj)(std::move(args)...); };
			}
		}

		// Default copy/move.
		//
		constexpr function_view(function_view&&) noexcept            = default;
		constexpr function_view(const function_view&)                = default;
		constexpr function_view& operator=(function_view&&) noexcept = default;
		constexpr function_view& operator=(const function_view&)     = default;

		// Validity check via cast to bool.
		//
		constexpr explicit operator bool() const { return fn != nullptr; }

		// Redirect to functor.
		//
		constexpr Ret operator()(Args... args) const { return fn(obj, std::forward<Args>(args)...); }
	};

	// Deduction guide.
	//
	template<typename F>
	function_view(F) -> function_view<typename function_traits<F>::normal_form>;
};