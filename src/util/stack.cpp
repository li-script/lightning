#include <util/stack.hpp>

#include <util/platform.hpp>

#include <cerrno>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#if LI_WINDOWS
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <windows.h>
#elif LI_OSX || LI_UNIX
	#if LI_OSX || defined(__linux__)
		#include <pthread.h>
	#endif
	#include <sys/mman.h>
#endif

namespace li::platform {
	namespace {
#if LI_WINDOWS
		std::error_code last_platform_error() noexcept { return std::error_code(static_cast<int>(GetLastError()), std::system_category()); }
#endif

#if LI_WINDOWS || LI_OSX || defined(__linux__)
		native_stack_bounds bounds_from_low_and_size(std::uintptr_t low, std::size_t size) noexcept {
			if (!low || !size || size > std::numeric_limits<std::uintptr_t>::max() - low)
				return {};
			return {low, low + size};
		}

		native_stack_bounds query_current_native_stack_bounds() noexcept {
	#if LI_WINDOWS
			// NT_TIB::StackLimit is only the committed low bound (it moves with the guard
			// page), so the thread's reserved range comes from GetCurrentThreadStackLimits.
			// A context switch replaces StackBase/StackLimit but not the thread's
			// reservation; when they disagree the TEB describes the active stack.
			const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
			if (!tib)
				return {};
			auto      low           = reinterpret_cast<std::uintptr_t>(tib->StackLimit);
			auto      high          = reinterpret_cast<std::uintptr_t>(tib->StackBase);
			ULONG_PTR reserved_low  = 0;
			ULONG_PTR reserved_high = 0;
			GetCurrentThreadStackLimits(&reserved_low, &reserved_high);
			if (reserved_low && std::uintptr_t(reserved_high) == high)
				low = std::uintptr_t(reserved_low);
			if (!low || low >= high)
				return {};
			return bounds_from_low_and_size(low, high - low);
	#elif LI_OSX
			const pthread_t thread = pthread_self();
			const auto      high   = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(thread));
			const auto      size   = pthread_get_stacksize_np(thread);
			if (!high || !size || size > high)
				return {};
			return bounds_from_low_and_size(high - size, size);
	#elif defined(__linux__)
			pthread_attr_t attributes;
			if (pthread_getattr_np(pthread_self(), &attributes) != 0)
				return {};

			void*       low    = nullptr;
			std::size_t size   = 0;
			const int   result = pthread_attr_getstack(&attributes, &low, &size);
			pthread_attr_destroy(&attributes);
			if (result != 0)
				return {};
			return bounds_from_low_and_size(reinterpret_cast<std::uintptr_t>(low), size);
	#else
			return {};
	#endif
		}
#endif
	};

	native_stack_bounds current_native_stack_bounds() noexcept {
#if LI_WINDOWS
		// Fiber switches replace these two TEB fields. Keep the per-thread cache coherent
		// with the active native stack while retaining a syscall-free hot path.
		static thread_local native_stack_bounds cached;
		const native_stack_bounds               current = query_current_native_stack_bounds();
		if (cached.low != current.low || cached.high != current.high)
			cached = current;
		return cached;
#elif LI_OSX || defined(__linux__)
		static thread_local const native_stack_bounds cached = query_current_native_stack_bounds();
		return cached;
#else
		return {};
#endif
	}

	native_stack::native_stack(std::size_t reserved_size) {
		if (reserved_size < initial_committed_size) [[unlikely]]
			throw std::invalid_argument("native stack reservation is smaller than its initial commit");

		std::error_code error;
		page_size_ = native_page_size(error);
		if (error) [[unlikely]]
			throw std::system_error(error, "query native stack page size");

		std::size_t rounded_reservation = 0;
		std::size_t rounded_initial     = 0;
		if (!round_to_page(reserved_size, page_size_, rounded_reservation) || !round_to_page(initial_committed_size, page_size_, rounded_initial)) [[unlikely]]
			throw std::length_error("native stack reservation is too large");
		if (rounded_reservation < rounded_initial) [[unlikely]]
			throw std::invalid_argument("native stack reservation is smaller than its initial commit");
		if (page_size_ > (std::numeric_limits<std::size_t>::max() - rounded_reservation) / 2) [[unlikely]]
			throw std::length_error("native stack mapping size overflows");

		const std::size_t mapping_size = rounded_reservation + 2 * page_size_;
		void*             allocation   = nullptr;
#if LI_WINDOWS
		allocation = VirtualAlloc(nullptr, mapping_size, MEM_RESERVE, PAGE_NOACCESS);
		if (!allocation) [[unlikely]]
			throw std::system_error(last_platform_error(), "reserve native stack");

		auto* const usable_end  = static_cast<std::byte*>(allocation) + page_size_ + rounded_reservation;
		void* const commit_base = usable_end - rounded_initial;
		if (!VirtualAlloc(commit_base, rounded_initial, MEM_COMMIT, PAGE_READWRITE)) [[unlikely]] {
			const auto error = last_platform_error();
			VirtualFree(allocation, 0, MEM_RELEASE);
			throw std::system_error(error, "commit native stack");
		}
#elif LI_OSX || LI_UNIX
	#ifdef MAP_ANONYMOUS
		constexpr int anonymous_mapping = MAP_ANONYMOUS;
	#else
		constexpr int anonymous_mapping = MAP_ANON;
	#endif
		allocation = mmap(nullptr, mapping_size, PROT_NONE, MAP_PRIVATE | anonymous_mapping, -1, 0);
		if (allocation == MAP_FAILED) [[unlikely]]
			throw std::system_error(std::error_code(errno, std::generic_category()), "reserve native stack");

		auto* const usable_end  = static_cast<std::byte*>(allocation) + page_size_ + rounded_reservation;
		void* const commit_base = usable_end - rounded_initial;
		if (mprotect(commit_base, rounded_initial, PROT_READ | PROT_WRITE) != 0) [[unlikely]] {
			const auto error = std::error_code(errno, std::generic_category());
			munmap(allocation, mapping_size);
			throw std::system_error(error, "commit native stack");
		}
#else
		throw std::system_error(std::make_error_code(std::errc::not_supported), "native stacks are not supported");
#endif

		mapping_        = static_cast<std::byte*>(allocation);
		mapping_size_   = mapping_size;
		reserved_size_  = rounded_reservation;
		committed_size_ = rounded_initial;
	}

	void native_stack::release() noexcept {
		if (mapping_) {
#if LI_WINDOWS
			VirtualFree(mapping_, 0, MEM_RELEASE);
#elif LI_OSX || LI_UNIX
			munmap(mapping_, mapping_size_);
#endif
		}
		mapping_        = nullptr;
		mapping_size_   = 0;
		reserved_size_  = 0;
		committed_size_ = 0;
		page_size_      = 0;
	}

	native_stack::~native_stack() noexcept { release(); }

	native_stack::native_stack(native_stack&& other) noexcept
		 : mapping_(std::exchange(other.mapping_, nullptr)),
			mapping_size_(std::exchange(other.mapping_size_, 0)),
			reserved_size_(std::exchange(other.reserved_size_, 0)),
			committed_size_(std::exchange(other.committed_size_, 0)),
			page_size_(std::exchange(other.page_size_, 0)) {}

	native_stack& native_stack::operator=(native_stack&& other) noexcept {
		if (this != &other) {
			release();
			mapping_        = std::exchange(other.mapping_, nullptr);
			mapping_size_   = std::exchange(other.mapping_size_, 0);
			reserved_size_  = std::exchange(other.reserved_size_, 0);
			committed_size_ = std::exchange(other.committed_size_, 0);
			page_size_      = std::exchange(other.page_size_, 0);
		}
		return *this;
	}

	void* native_stack::data() noexcept { return mapping_ ? static_cast<void*>(mapping_ + page_size_ + reserved_size_ - committed_size_) : nullptr; }

	const void* native_stack::data() const noexcept {
		return mapping_ ? static_cast<const void*>(mapping_ + page_size_ + reserved_size_ - committed_size_) : nullptr;
	}

	void* native_stack::end() noexcept { return mapping_ ? static_cast<void*>(mapping_ + page_size_ + reserved_size_) : nullptr; }

	const void* native_stack::end() const noexcept { return mapping_ ? static_cast<const void*>(mapping_ + page_size_ + reserved_size_) : nullptr; }

	std::span<const std::byte> native_stack::guard_range() const noexcept {
		return mapping_ ? std::span<const std::byte>(mapping_, page_size_) : std::span<const std::byte>{};
	}

	std::span<const std::byte> native_stack::upper_guard_range() const noexcept {
		return mapping_ ? std::span<const std::byte>(mapping_ + page_size_ + reserved_size_, page_size_) : std::span<const std::byte>{};
	}

	bool native_stack::contains(const void* address) const noexcept {
		if (!mapping_ || !address)
			return false;
		const auto candidate = reinterpret_cast<std::uintptr_t>(address);
		const auto first     = reinterpret_cast<std::uintptr_t>(mapping_ + page_size_);
		const auto last      = first + reserved_size_;
		return candidate >= first && candidate < last;
	}

	bool native_stack::grow(std::size_t new_size) noexcept {
		if (!mapping_)
			return false;
		if (new_size <= committed_size_)
			return true;
		if (new_size > reserved_size_)
			return false;

		std::size_t rounded_size = 0;
		if (!round_to_page(new_size, page_size_, rounded_size) || rounded_size > reserved_size_) [[unlikely]]
			return false;

		auto* const       usable_end = mapping_ + page_size_ + reserved_size_;
		void* const       grow_base  = usable_end - rounded_size;
		const std::size_t size       = rounded_size - committed_size_;
#if LI_WINDOWS
		if (!VirtualAlloc(grow_base, size, MEM_COMMIT, PAGE_READWRITE)) [[unlikely]]
			return false;
#elif LI_OSX || LI_UNIX
		if (mprotect(grow_base, size, PROT_READ | PROT_WRITE) != 0) [[unlikely]]
			return false;
#else
		return false;
#endif
		committed_size_ = rounded_size;
		return true;
	}
};
