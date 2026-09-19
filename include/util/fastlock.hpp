#pragma once
#include <atomic>
#include <cassert>
#include <limits>
#include <system_error>
#include <util/common.hpp>

#if LI_MS_EXTS
	#include <intrin.h>
#endif
#if LI_EMSCRIPTEN
	#include <pthread.h>
#endif

namespace li::util {
	// Recursive VM mutex with bounded initial spinning and C++20 parking; acquisition is not FIFO.
	// Destruction requires no owners or waiters, as with std::recursive_mutex.
	class fastlock {
		// 0 = free, 1 = notifying before release, otherwise a thread pointer with a waiter bit.
		static constexpr uintptr_t parked = 1;
		std::atomic<uintptr_t>     owner  = 0;
		size_t                     depth  = 0;  // Accessed only by the owning thread.

		static_assert(std::atomic<uintptr_t>::is_always_lock_free);

		// Native ABI thread pointers are aligned and avoid TLS resolvers and thread-id syscalls.
		LI_INLINE LI_CONST static uintptr_t thread_id() noexcept {
#if LI_WINDOWS && LI_ARCH_X86 && !LI_32
	#if LI_MS_EXTS
			return __readgsqword(0x30);
	#else
			uintptr_t tid;
			asm("movq %%gs:0x30, %0" : "=r"(tid));
			return tid;
	#endif
#elif LI_WINDOWS && LI_ARCH_ARM
	#if LI_MS_EXTS
			return __getReg(18);  // Windows reserves x18 for the TEB.
	#else
			uintptr_t tid;
			asm("mov %0, x18" : "=r"(tid));
			return tid;
	#endif
#elif LI_OSX && LI_ARCH_X86 && !LI_32
			uintptr_t tid;
			asm("movq %%gs:0, %0" : "=r"(tid));
			return tid;
#elif LI_OSX && LI_ARCH_ARM
			uintptr_t tid;
			asm("mrs %0, tpidrro_el0" : "=r"(tid));
			return tid;  // Darwin's aligned TSD base, not TPIDR_EL0 (CPU information).
#elif defined(__linux__) && (LI_ARCH_X86 || LI_ARCH_ARM)
			return reinterpret_cast<uintptr_t>(__builtin_thread_pointer());
#elif LI_EMSCRIPTEN
			return reinterpret_cast<uintptr_t>(pthread_self());
#else
	#error "fastlock needs a native thread pointer for this target."
#endif
		}

		LI_INLINE static void pause() noexcept {
#if LI_ARCH_X86 && LI_MS_EXTS
			_mm_pause();
#elif LI_ARCH_X86
			__builtin_ia32_pause();
#elif LI_ARCH_ARM && LI_MS_EXTS
			__yield();
#elif LI_ARCH_ARM
			asm volatile("yield");
#endif
		}

		LI_INLINE bool acquire(uintptr_t tid) noexcept {
			assert(tid != 0 && !(tid & parked));
			uintptr_t current = owner.load(std::memory_order::relaxed);
			if ((current & ~parked) == tid) {
				if (depth == std::numeric_limits<size_t>::max()) [[unlikely]]
					return false;
				++depth;
				return true;
			}
			if (current == 0 && owner.compare_exchange_strong(current, tid, std::memory_order::acquire, std::memory_order::relaxed)) {
				depth = 1;
				return true;
			}
			return false;
		}

		LI_NOINLINE void lock_slow(uintptr_t tid) {
			if ((owner.load(std::memory_order::relaxed) & ~parked) == tid) [[unlikely]]
				throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));

			// Poll with loads, not RMWs; atomic::wait supplies further library-tuned backoff.
			for (unsigned spin = 0; spin != 16; ++spin) {
				pause();
				uintptr_t current = owner.load(std::memory_order::relaxed);
				if (current == 0 && owner.compare_exchange_strong(current, tid, std::memory_order::acquire, std::memory_order::relaxed))
					return;
			}

			uintptr_t current = owner.load(std::memory_order::relaxed);
			for (;;) {
				if (current == 0) {
					// The previous release notified every sleeper; new waiters install their own flag.
					if (owner.compare_exchange_weak(current, tid, std::memory_order::acquire, std::memory_order::relaxed))
						return;
				} else if (current == parked) {
					// The releasing owner is notifying; it must finish before anyone can acquire.
					pause();
					current = owner.load(std::memory_order::relaxed);
				} else {
					if (!(current & parked)) {
						if (!owner.compare_exchange_weak(current, current | parked, std::memory_order::relaxed, std::memory_order::relaxed))
							continue;
						current |= parked;
					}
					// Only sleep on a flagged owner: even an ABA back to this word owes a wakeup.
					owner.wait(current, std::memory_order::relaxed);
					current = owner.load(std::memory_order::relaxed);
				}
			}
		}

	  public:
		fastlock() noexcept                  = default;
		fastlock(const fastlock&)            = delete;
		fastlock& operator=(const fastlock&) = delete;

		// Acquires the VM lock recursively; throws system_error if the recursion count is exhausted.
		LI_INLINE void lock() {
			uintptr_t tid = thread_id();
			if (!acquire(tid)) [[unlikely]] {
				lock_slow(tid);
				depth = 1;
			}
		}

		// Acquires without waiting, or returns false on contention or recursion-count exhaustion.
		LI_INLINE bool try_lock() noexcept { return acquire(thread_id()); }

		// Releases one level; the calling thread must own the lock.
		LI_INLINE void unlock() noexcept {
			if (--depth == 0) {
				// Arbitrate with flag setters, then notify before making the lock acquirable.
				// Nothing may touch this object after the release: the next owner may destroy it.
				const uintptr_t previous = owner.exchange(parked, std::memory_order::relaxed);
				assert((previous & ~parked) == thread_id());
				if (previous & parked) [[unlikely]]
					owner.notify_all();
				owner.store(0, std::memory_order::release);
			}
		}
	};
};