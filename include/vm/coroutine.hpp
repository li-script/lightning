#pragma once
#include <cstddef>
#include <cstdint>
#include <vm/types.hpp>

namespace li {
	struct vm;
	struct coroutine_state;
	struct next_result;

	// Clang's ASan use-after-return instrumentation places the interpreter in stack
	// size class 6: 64 << 6 == 4 KiB. Reserve sixteen such frames before entering
	// another script frame, and start sanitizer fibers with four entry headrooms
	// committed so their bootstrap and first calls cannot consume the guard margin.
	inline constexpr std::size_t coroutine_native_stack_reservation = 1024 * 1024;
	// Windows raises STATUS_STACK_OVERFLOW before the reservation is exhausted: the
	// guard page plus the thread's stack guarantee sit above DeallocationStack, so
	// a 16 KiB margin lands on that boundary. Keep the larger margin there too.
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer) || defined(_WIN32)
	inline constexpr std::size_t coroutine_native_stack_headroom      = 64 * 1024;
	inline constexpr std::size_t coroutine_native_stack_initial_floor = 4 * coroutine_native_stack_headroom;
#else
	inline constexpr std::size_t coroutine_native_stack_headroom      = 16 * 1024;
	inline constexpr std::size_t coroutine_native_stack_initial_floor = 64 * 1024;
#endif

	enum class coroutine_status : uint8_t {
		created,
		running,
		suspended,
		dead,
	};

	// Coroutine objects use the ordinary object tag and are identified by their VM-local
	// class. These observers borrow their arguments and never mutate the coroutine.
	bool             is_coroutine(const vm* L, any_t value) noexcept;
	coroutine_status get_coroutine_status(const vm* L, any_t value) noexcept;

	// Advances a coroutine through the shared iterator protocol. The returned
	// value is owned and done is independent of a yielded nil.
	next_result coroutine_next(vm* L, any_t value);

	namespace lib {
		// Installs coroutine.create/resume/yield/status/close/next. Instance methods are
		// the same native functions stored on the coroutine class and therefore do not
		// capture or retain an individual coroutine.
		void register_coroutine(vm* L);
	}
}
