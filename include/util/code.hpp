#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>
#include <type_traits>
#include <util/common.hpp>
#include <utility>

namespace li::platform {
#if LI_ARCH_X86
	// Native x86 INT3 instruction bytes.
	inline constexpr std::array<std::byte, 1> breakpoint_bytes = {std::byte{0xcc}};
#elif LI_ARCH_ARM
	// Native AArch64 BRK #0 instruction word.
	inline constexpr std::uint32_t breakpoint_word = 0xd4200000u;

	// Native AArch64 BRK #0 instruction bytes in instruction-stream (little-endian) order.
	inline constexpr std::array<std::byte, 4> breakpoint_bytes = {std::byte{0x00}, std::byte{0x00}, std::byte{0x20}, std::byte{0xd4}};
#endif

#if defined(__clang__)
	#define LI_GENERATED_CODE_CALL __attribute__((no_sanitize("function")))
#else
	#define LI_GENERATED_CODE_CALL
#endif

	// Invokes machine-generated code that has no Clang function type cookie. Unsafe: Function must
	// exactly match the generated entry's ABI and signature, and entry must remain executable and
	// alive until the call returns. Native C++ function pointers must be called normally so their
	// function-sanitizer checks remain enabled.
	template<typename Function, typename... Args>
	LI_GENERATED_CODE_CALL decltype(auto) invoke_generated_code(const void* entry, Args&&... args) {
		static_assert(std::is_pointer_v<Function> && std::is_function_v<std::remove_pointer_t<Function>>);
		return reinterpret_cast<Function>(const_cast<void*>(entry))(std::forward<Args>(args)...);
	}

#undef LI_GENERATED_CODE_CALL

	// Proof that no thread can enter or still be executing code in a mapping reopened for writing.
	struct inactive_code_t {
		explicit constexpr inactive_code_t() noexcept = default;
	};

	// Pass to reopen_for_write only after making the mapping unreachable by executing threads.
	inline constexpr inactive_code_t inactive_code{};

	// Owns one page-rounded executable-code mapping, separately from any language object.
	// The owner is move-only and is not reference counted. A caller publishing an entry point must
	// retain this object for the entire execution, and must externally synchronize publishing,
	// reopening, moving, and destruction against every executing thread.
	class code_memory {
		std::byte*  address_         = nullptr;
		std::size_t capacity_        = 0;
		std::size_t allocation_size_ = 0;
		std::size_t page_size_       = 0;
		bool        writable_        = false;
		bool        published_       = false;

		void release() noexcept;

	  public:
		// Constructs an empty, non-owning object.
		code_memory() noexcept = default;

		// Unmaps the owned allocation, whether or not it has been published.
		~code_memory() noexcept;

		// Mapping ownership cannot be copied.
		code_memory(const code_memory&)            = delete;
		code_memory& operator=(const code_memory&) = delete;

		// Transfers mapping ownership and leaves other empty.
		code_memory(code_memory&& other) noexcept;

		// Unmaps the current mapping, transfers ownership, and leaves other empty.
		code_memory& operator=(code_memory&& other) noexcept;

		// Allocates at least capacity writable bytes. The allocation is never writable and executable
		// at the same time on Linux or Windows. Returns an empty object and sets error on failure.
		[[nodiscard]] static code_memory allocate(std::size_t capacity, std::error_code& error) noexcept;

		// Reports whether this object owns a mapping.
		[[nodiscard]] explicit operator bool() const noexcept { return address_ != nullptr; }

		// Returns the requested writable byte capacity.
		[[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

		// Returns the page-rounded byte size of the OS allocation.
		[[nodiscard]] std::size_t allocation_size() const noexcept { return allocation_size_; }

		// Returns the actual OS page size used to round this allocation.
		[[nodiscard]] std::size_t page_size() const noexcept { return page_size_; }

		// Returns true only after a successful publish following the most recent writable phase.
		[[nodiscard]] bool published() const noexcept { return published_; }

		// Returns the mapping base. It may be called as code only while published, and the caller must
		// retain this object until every invocation has returned.
		[[nodiscard]] const void* data() const noexcept { return address_; }

		// Copies bytes into a writable mapping. The entire [offset, offset + bytes.size()) range must
		// lie within capacity; an empty write at capacity is valid. Returns an error instead of writing
		// if the mapping is empty, published, or out of bounds.
		[[nodiscard]] std::error_code write(std::size_t offset, std::span<const std::byte> bytes) noexcept;

		// Makes the mapping executable and non-writable where the OS supports page protections, then
		// invalidates the instruction cache. Every call invalidates the cache, including repeat calls.
		[[nodiscard]] std::error_code publish() noexcept;

		// Makes a published mapping writable and non-executable on Linux and Windows. Passing the tag
		// is an explicit promise that no thread can execute this mapping until the next successful
		// publish. Apple MAP_JIT mappings remain protected by per-thread write-protect toggles.
		[[nodiscard]] std::error_code reopen_for_write(inactive_code_t) noexcept;
	};
};
