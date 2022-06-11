#pragma once
#include <util/common.hpp>
#include <atomic>
#include <mutex>
#include <thread>

// Defines the fast recursive spinlock we use for guarding the VM.
//
namespace li::util {
	// GS/FS Base reading for x86/x86-64.
	//
#if LI_ARCH_X86
	LI_INLINE LI_CONST static uintptr_t read_gsbase() {
#if !LI_32
	#if LI_MSVC
		return _readgsbase_u64();
	#else
		uint64_t value;
		asm( "rdgsbase %0" : "=r" ( value ) :: );
		return value;
	#endif
#else
	#if LI_MSVC
		return _readgsbase_u32();
	#else
		uint32_t value;
		asm( "rdgsbase %0" : "=r" ( value ) :: );
		return value;
	#endif
#endif
	}
	LI_INLINE LI_CONST static uintptr_t read_fsbase() {
#if !LI_32
	#if LI_MSVC
		return _readfsbase_u64();
	#else
		uint64_t value;
		asm( "rdfsbase %0" : "=r" ( value ) :: );
		return value;
	#endif
#else
	#if LI_MSVC
		return _readfsbase_u32();
	#else
		uint32_t value;
		asm( "rdfsbase %0" : "=r" ( value ) :: );
		return value;
	#endif
#endif
	}
#endif

	// Gets a pseudo thread identifier in a fast way.
	//
#if LI_ARCH_X86
	LI_INLINE static uintptr_t read_fast_thread_id() {
	#if !LI_32 && (LI_WINDOWS || LI_OSX)
		return read_gsbase();
	#else
		return read_fsbase();
	#endif
	}
#else
	inline thread_local char __id;
	LI_INLINE static uintptr_t read_fast_thread_id() {
		return (uintptr_t) &__id;
	}
#endif

	// Yields the CPU.
	//
	LI_INLINE static void yield_cpu() {
#if LI_ARCH_X86 && LI_CLANG
		__builtin_ia32_pause();
#elif LI_ARCH_X86 && LI_GNU
      asm volatile("pause" ::: "memory");
#elif LI_ARCH_X86 && LI_MSVC
		_mm_pause();
#elif LI_ARCH_ARM
		asm volatile("yield" ::: "memory");
#endif
	}

	// Finally define the lock.
	//
	struct fastlock {
		std::atomic<uintptr_t> owner  = 0;
		std::atomic<uint16_t>  signal = 0;
		uint16_t               depth  = 0;

		LI_INLINE bool try_lock_fast(uintptr_t tid) {
			while (true) {
				uintptr_t expected = 0;
				if (!owner.compare_exchange_strong(expected, tid) && expected != tid) {
					uint8_t yield_counter = 0;
					while (expected != 0) {
						if (++yield_counter > 128) {
							return false;
						}
						yield_cpu();
						expected = owner.load(std::memory_order::relaxed);
					}
				}
			}
			return true;
		}
		LI_COLD void try_lock_slow(uintptr_t tid) {
			signal.store(1, std::memory_order::relaxed);
			while (true) {
				uintptr_t expected = 0;
				if (!owner.compare_exchange_strong(expected, tid) && expected != tid) {
					while (expected != 0) {
						signal.store(1, std::memory_order::relaxed);
						owner.wait(expected);
						expected = owner.load(std::memory_order::relaxed);
					}
				}
			}
		}
		LI_INLINE void lock() {
			uintptr_t tid = read_fast_thread_id();
			if (!try_lock_fast(tid)) [[unlikely]] {
				try_lock_slow(tid);
			}
			++depth;
		}
		LI_INLINE void unlock() {
			if (!--depth) {
				owner.store(0, std::memory_order::release);
				if (signal.load(std::memory_order::relaxed) != 0) [[unlikely]] {
					owner.notify_one();
					signal.store(0, std::memory_order::relaxed);
				}
			}
		}
		LI_INLINE bool locked() const { return owner.load(std::memory_order::relaxed) != 0; }
	};
};