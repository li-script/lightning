#include <util/code.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace code_memory_test {
#if LI_ARCH_X86
	static_assert(li::platform::breakpoint_bytes == std::array{std::byte{0xcc}});
#elif LI_ARCH_ARM
	static_assert(li::platform::breakpoint_word == 0xd4200000u);
	static_assert(li::platform::breakpoint_bytes == std::array{std::byte{0x00}, std::byte{0x00}, std::byte{0x20}, std::byte{0xd4}});
#endif

	void require(bool condition, std::string_view message) {
		if (!condition)
			throw std::runtime_error(std::string(message));
	}

	void require_success(const std::error_code& error, std::string_view operation) {
		if (error)
			throw std::system_error(error, std::string(operation));
	}

	constexpr auto return_42_code() {
#if LI_ARCH_X86
		// mov eax, 42; ret
		return std::array{std::byte{0xb8}, std::byte{0x2a}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xc3}};
#elif LI_ARCH_ARM
		// mov w0, #42; ret
		return std::array{std::byte{0x40}, std::byte{0x05}, std::byte{0x80}, std::byte{0x52}, std::byte{0xc0}, std::byte{0x03}, std::byte{0x5f}, std::byte{0xd6}};
#else
		return std::array<std::byte, 0>{};
#endif
	}

	void test_failures_and_bounds() {
		std::error_code error;
		auto            empty = li::platform::code_memory::allocate(0, error);
		require(!empty, "zero-size allocation must be empty");
		require(error == std::errc::invalid_argument, "zero-size allocation must report invalid_argument");

		auto overflow = li::platform::code_memory::allocate(std::numeric_limits<std::size_t>::max(), error);
		require(!overflow, "overflowing allocation must be empty");
		require(error == std::errc::value_too_large, "overflowing allocation must report value_too_large");

		auto memory = li::platform::code_memory::allocate(1, error);
		require_success(error, "allocate boundary-test mapping");
		require(memory.capacity() == 1, "capacity must preserve the requested byte count");
		require(memory.page_size() != 0, "page size must be reported");
		require(memory.allocation_size() >= memory.capacity(), "allocation must cover capacity");
		require(memory.allocation_size() % memory.page_size() == 0, "allocation must be page rounded");

		const std::array one_byte{std::byte{0xc3}};
		require(memory.write(1, one_byte) == std::errc::result_out_of_range, "write at capacity must reject non-empty input");
		require(memory.write(2, {}) == std::errc::result_out_of_range, "empty write beyond capacity must fail");
		require_success(memory.write(1, {}), "empty boundary write");
		require_success(memory.write(0, one_byte), "in-bounds write");
		require_success(memory.publish(), "publish boundary-test mapping");
		require(memory.published(), "successful publish must update state");
		require(memory.write(0, one_byte) == std::errc::operation_not_permitted, "published mapping must reject writes");
	}

	void test_publish_execute_move_and_reopen() {
		constexpr auto code = return_42_code();
		static_assert(!code.empty(), "code-memory smoke test needs native return-42 instructions");

		std::error_code error;
		auto            memory = li::platform::code_memory::allocate(code.size(), error);
		require_success(error, "allocate executable mapping");
		require_success(memory.write(0, code), "write return-42 function");
		require_success(memory.publish(), "publish return-42 function");
		require_success(memory.publish(), "republish and invalidate cache");

		li::platform::code_memory moved(std::move(memory));
		require(!memory, "move construction must empty the source");
		require(moved.published(), "move construction must retain publication state");

		using return_function = int (*)();
		require(li::platform::invoke_generated_code<return_function>(moved.data()) == 42, "published native function must execute and return 42");

		auto replacement = li::platform::code_memory::allocate(code.size(), error);
		require_success(error, "allocate move-assignment destination");
		replacement = std::move(moved);
		require(!moved, "move assignment must empty the source");
		require(li::platform::invoke_generated_code<return_function>(replacement.data()) == 42, "move assignment must preserve executable mapping ownership");

		require_success(replacement.reopen_for_write(li::platform::inactive_code), "reopen inactive mapping");
		require(!replacement.published(), "reopened mapping must no longer be published");
		require_success(replacement.write(0, code), "rewrite inactive mapping");
		require_success(replacement.publish(), "republish rewritten mapping");
		require(
			 li::platform::invoke_generated_code<return_function>(replacement.data()) == 42, "republished native function must execute after cache invalidation");
	}

	void test_unpublished_destruction() {
		std::error_code error;
		{
			auto unpublished = li::platform::code_memory::allocate(8, error);
			require_success(error, "allocate unpublished mapping");
			require(static_cast<bool>(unpublished), "unpublished allocation must be owned");
		}
	}
};

int main() {
	code_memory_test::test_failures_and_bounds();
	code_memory_test::test_publish_execute_move_and_reopen();
	code_memory_test::test_unpublished_destruction();
	return 0;
}
