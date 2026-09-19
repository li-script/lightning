#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace li::platform {
	// Inclusive architectural stack-pointer limits for the current native stack. An empty
	// result means that the platform could not establish trustworthy bounds.
	struct native_stack_bounds {
		std::uintptr_t low  = 0;
		std::uintptr_t high = 0;

		[[nodiscard]] explicit operator bool() const noexcept { return low != 0 && low < high; }
	};

	// Queries the current thread's OS stack limits. macOS/Linux limits are cached per thread
	// after the first query; Windows tracks the active TEB limits so native fiber switches remain
	// visible without a system call.
	[[nodiscard]] native_stack_bounds current_native_stack_bounds() noexcept;

	// Owns a fixed virtual-address range for a downward-growing native stack. The usable
	// reservation is bounded by inaccessible pages and only its high end is committed.
	class native_stack {
		std::byte*  mapping_        = nullptr;
		std::size_t mapping_size_   = 0;
		std::size_t reserved_size_  = 0;
		std::size_t committed_size_ = 0;
		std::size_t page_size_      = 0;

		void release() noexcept;

	  public:
		static constexpr std::size_t initial_committed_size = 64 * 1024;

		// Constructs an empty, non-owning stack.
		native_stack() noexcept = default;

		// Reserves at least reserved_size usable bytes without making their addresses movable.
		// The initial high 64 KiB (rounded to an OS page) is made read/write. Throws
		// std::invalid_argument, std::length_error, or std::system_error on failure.
		explicit native_stack(std::size_t reserved_size);

		~native_stack() noexcept;

		native_stack(const native_stack&)            = delete;
		native_stack& operator=(const native_stack&) = delete;

		native_stack(native_stack&& other) noexcept;
		native_stack& operator=(native_stack&& other) noexcept;

		[[nodiscard]] explicit operator bool() const noexcept { return mapping_ != nullptr; }

		// Lowest currently committed address and the stable, 16-byte-aligned stack top.
		[[nodiscard]] void*       data() noexcept;
		[[nodiscard]] const void* data() const noexcept;
		[[nodiscard]] void*       end() noexcept;
		[[nodiscard]] const void* end() const noexcept;

		[[nodiscard]] std::size_t committed_size() const noexcept { return committed_size_; }
		[[nodiscard]] std::size_t reserved_size() const noexcept { return reserved_size_; }
		[[nodiscard]] std::size_t page_size() const noexcept { return page_size_; }

		// The permanent lower and upper inaccessible guard pages. The spans describe address
		// ranges only; their contents must never be accessed.
		[[nodiscard]] std::span<const std::byte> guard_range() const noexcept;
		[[nodiscard]] std::span<const std::byte> upper_guard_range() const noexcept;

		// Reports whether address lies in the usable reservation, including its currently
		// uncommitted portion but excluding both guards.
		[[nodiscard]] bool contains(const void* address) const noexcept;

		// While execution is suspended, commits enough pages to provide at least new_size
		// usable bytes. Existing addresses never change. Returns false if the request cannot
		// fit or the OS cannot commit it; shrinking is a successful no-op.
		[[nodiscard]] bool grow(std::size_t new_size) noexcept;
	};
};
