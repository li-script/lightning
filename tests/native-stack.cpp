#include <util/common.hpp>
#include <util/context.hpp>
#include <util/stack.hpp>
#include <vm/state.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#if LI_WINDOWS
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <windows.h>
#elif LI_OSX
	#include <mach/mach.h>
	#include <mach/mach_vm.h>
	#include <sys/mman.h>
#elif LI_UNIX
	#include <sys/mman.h>
#endif

namespace native_stack_test {
	void require(bool condition, std::string_view message) {
		if (!condition)
			throw std::runtime_error(std::string(message));
	}

	std::uintptr_t address(const void* pointer) { return reinterpret_cast<std::uintptr_t>(pointer); }

	bool inaccessible_mapping_at(const void* pointer) {
#if LI_WINDOWS
		MEMORY_BASIC_INFORMATION info{};
		if (VirtualQuery(pointer, &info, sizeof(info)) != sizeof(info))
			return false;
		return info.State == MEM_RESERVE || (info.State == MEM_COMMIT && (info.Protect & 0xffu) == PAGE_NOACCESS);
#elif LI_OSX
		mach_vm_address_t              region_address = address(pointer);
		mach_vm_size_t                 region_size    = 0;
		vm_region_basic_info_data_64_t info{};
		mach_msg_type_number_t         count       = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t                    object_name = MACH_PORT_NULL;
		const kern_return_t            result      = mach_vm_region(
			 mach_task_self(), &region_address, &region_size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object_name);
		if (object_name != MACH_PORT_NULL)
			mach_port_deallocate(mach_task_self(), object_name);
		return result == KERN_SUCCESS && region_address <= address(pointer) && address(pointer) - region_address < region_size && info.protection == VM_PROT_NONE;
#elif defined(__linux__)
		std::ifstream mappings("/proc/self/maps");
		std::string   line;
		while (std::getline(mappings, line)) {
			unsigned long long first          = 0;
			unsigned long long last           = 0;
			char               permissions[5] = {};
			if (std::sscanf(line.c_str(), "%llx-%llx %4s", &first, &last, permissions) != 3)
				continue;
			const auto candidate = static_cast<unsigned long long>(address(pointer));
			if (candidate >= first && candidate < last)
				return permissions[0] == '-' && permissions[1] == '-' && permissions[2] == '-';
		}
		return false;
#else
		// POSIX has no portable protection-query API. mincore still verifies that the guard
		// address belongs to the live OS mapping; stack.cpp created that mapping as PROT_NONE.
	#if __APPLE__
		char resident = 0;
	#else
		unsigned char resident = 0;
	#endif
		return mincore(const_cast<void*>(pointer), 1, &resident) == 0;
#endif
	}

	bool unmapped_at(std::uintptr_t pointer, std::size_t page_size) {
#if LI_WINDOWS
		MEMORY_BASIC_INFORMATION info{};
		return VirtualQuery(reinterpret_cast<const void*>(pointer), &info, sizeof(info)) == sizeof(info) && info.State == MEM_FREE;
#elif LI_OSX
		(void) page_size;
		mach_vm_address_t              region_address = pointer;
		mach_vm_size_t                 region_size    = 0;
		vm_region_basic_info_data_64_t info{};
		mach_msg_type_number_t         count       = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t                    object_name = MACH_PORT_NULL;
		const kern_return_t            result      = mach_vm_region(
			 mach_task_self(), &region_address, &region_size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object_name);
		if (object_name != MACH_PORT_NULL)
			mach_port_deallocate(mach_task_self(), object_name);
		return result == KERN_INVALID_ADDRESS || (result == KERN_SUCCESS && region_address > pointer);
#elif LI_UNIX
		unsigned char resident = 0;
		errno                  = 0;
		return mincore(reinterpret_cast<void*>(pointer), page_size, &resident) != 0 && errno == ENOMEM;
#else
		(void) pointer;
		(void) page_size;
		return true;
#endif
	}

	void test_current_thread_bounds() {
		const li::platform::native_stack_bounds first = li::platform::current_native_stack_bounds();
#if LI_WINDOWS || LI_OSX || defined(__linux__)
		require(static_cast<bool>(first), "current native stack bounds must be available");
		const li::platform::native_stack_bounds cached = li::platform::current_native_stack_bounds();
		require(cached.low == first.low && cached.high == first.high, "current native stack bounds must remain stable");
	#if LI_CONTEXT_SUPPORTED
		const auto stack_pointer = li::platform::current_stack_pointer();
		require(stack_pointer >= first.low && stack_pointer <= first.high, "architectural stack pointer must lie within current native stack bounds");
	#endif

		li::vm* L = li::vm::create();
		require(L != nullptr, "VM allocation failed");
	#if LI_CONTEXT_SUPPORTED
		require(L->native_stack_headroom_available(), "ordinary main-thread entry must have native stack headroom");
		const std::size_t full_stack_span = first.high - first.low;
		require(!L->native_stack_headroom_available(full_stack_span), "requirement larger than current headroom must be rejected");
	#endif
		require(!L->native_stack_headroom_available(std::numeric_limits<std::size_t>::max()), "overflowing native frame requirement must be rejected");
		L->close();
#else
		require(!first, "unsupported platforms must report unknown native stack bounds");
#endif
	}

	void test_invalid_sizes_and_empty_state() {
		li::platform::native_stack empty;
		require(!empty, "default stack must be empty");
		require(empty.data() == nullptr && empty.end() == nullptr, "empty stack must not expose addresses");
		require(empty.committed_size() == 0 && empty.reserved_size() == 0, "empty stack sizes must be zero");
		require(empty.guard_range().empty() && empty.upper_guard_range().empty(), "empty stack guards must be empty");
		require(!empty.contains(nullptr), "empty stack must contain no addresses");
		require(!empty.grow(li::platform::native_stack::initial_committed_size), "empty stack cannot grow");

		bool rejected_small = false;
		try {
			li::platform::native_stack too_small(li::platform::native_stack::initial_committed_size - 1);
		} catch (const std::invalid_argument&) {
			rejected_small = true;
		}
		require(rejected_small, "reservation smaller than the initial commit must be rejected");

		bool rejected_overflow = false;
		try {
			li::platform::native_stack too_large(std::numeric_limits<std::size_t>::max());
		} catch (const std::length_error&) {
			rejected_overflow = true;
		}
		require(rejected_overflow, "overflowing reservation must be rejected");
	}

	void test_layout_growth_and_guards() {
		constexpr std::size_t      requested_reservation = 1024 * 1024;
		li::platform::native_stack stack(requested_reservation);
		require(static_cast<bool>(stack), "stack reservation must be owned");
		require(stack.page_size() != 0, "OS page size must be recorded");
		require(stack.reserved_size() >= requested_reservation, "usable reservation must cover the request");
		require(stack.reserved_size() % stack.page_size() == 0, "usable reservation must be page rounded");
		require(stack.committed_size() >= li::platform::native_stack::initial_committed_size, "initial usable stack must cover 64 KiB");
		require(stack.committed_size() % stack.page_size() == 0, "initial commit must be page rounded");
		require(address(stack.end()) % 16 == 0, "stack top must be 16-byte aligned");
		require(address(stack.end()) - address(stack.data()) == stack.committed_size(), "data/end must delimit the committed range");

		const auto lower_guard = stack.guard_range();
		const auto upper_guard = stack.upper_guard_range();
		require(lower_guard.size() == stack.page_size(), "lower guard must occupy one OS page");
		require(upper_guard.size() == stack.page_size(), "upper guard must occupy one OS page");
		require(address(lower_guard.data()) + lower_guard.size() == address(stack.end()) - stack.reserved_size(), "lower guard must precede usable reservation");
		require(address(upper_guard.data()) == address(stack.end()), "upper guard must follow the stable stack top");
		require(inaccessible_mapping_at(lower_guard.data()), "lower guard must be OS-inaccessible");
		require(inaccessible_mapping_at(upper_guard.data()), "upper guard must be OS-inaccessible");

		const auto usable_begin = address(stack.end()) - stack.reserved_size();
		require(stack.contains(reinterpret_cast<const void*>(usable_begin)), "reservation must contain its first usable byte");
		require(stack.contains(reinterpret_cast<const void*>(address(stack.end()) - 1)), "reservation must contain its final usable byte");
		require(!stack.contains(reinterpret_cast<const void*>(usable_begin - 1)), "reservation must exclude lower guard");
		require(!stack.contains(stack.end()), "reservation must exclude upper guard");

		auto* const old_data               = static_cast<std::byte*>(stack.data());
		auto* const old_end                = static_cast<std::byte*>(stack.end());
		old_data[0]                        = std::byte{0x35};
		old_end[-1]                        = std::byte{0x7a};
		const std::size_t old_committed    = stack.committed_size();
		const std::size_t requested_growth = old_committed + 2 * stack.page_size();
		require(stack.grow(requested_growth), "in-reservation growth must succeed");
		require(stack.committed_size() >= requested_growth, "growth must cover requested usable size");
		require(stack.end() == old_end, "growth must preserve the stable stack top");
		require(address(stack.data()) < address(old_data), "downward growth must extend toward lower addresses");
		require(old_data[0] == std::byte{0x35} && old_end[-1] == std::byte{0x7a}, "growth must preserve bytes and existing addresses");
		require(stack.grow(old_committed), "shrink request must be a successful no-op");
		require(stack.committed_size() >= requested_growth, "grow must never shrink the committed range");
		require(!stack.grow(stack.reserved_size() + 1), "growth beyond reservation must fail");
		require(stack.end() == old_end, "failed growth must not move the stack");
		require(stack.guard_range().data() == lower_guard.data(), "growth must retain the lower guard address");
		require(stack.upper_guard_range().data() == upper_guard.data(), "growth must retain the upper guard address");
		require(inaccessible_mapping_at(stack.guard_range().data()), "growth must retain lower guard protection");
		require(inaccessible_mapping_at(stack.upper_guard_range().data()), "growth must retain upper guard protection");
	}

	void test_move_and_release() {
		li::platform::native_stack source(1024 * 1024);
		const auto                 source_guard = address(source.guard_range().data());
		const auto                 source_top   = source.end();
		const auto                 page_size    = source.page_size();

		li::platform::native_stack moved(std::move(source));
		require(!source, "move construction must empty source");
		require(moved.end() == source_top, "move construction must preserve mapping address");

		li::platform::native_stack destination(1024 * 1024);
		const auto                 replaced_guard = address(destination.guard_range().data());
		destination                               = std::move(moved);
		require(!moved, "move assignment must empty source");
		require(destination.end() == source_top, "move assignment must preserve mapping address");
		require(unmapped_at(replaced_guard, page_size), "move assignment must release its previous reservation");

		destination = li::platform::native_stack{};
		require(!destination, "move assignment from empty must release ownership");
		require(unmapped_at(source_guard, page_size), "released stack reservation must be returned to the OS");
	}
};

int main() {
	try {
		native_stack_test::test_current_thread_bounds();
		native_stack_test::test_invalid_sizes_and_empty_state();
		native_stack_test::test_layout_growth_and_guards();
		native_stack_test::test_move_and_release();
	} catch (const std::exception& error) {
		std::fprintf(stderr, "native stack probe: %s\n", error.what());
		return 1;
	}
	return 0;
}
