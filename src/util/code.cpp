#include <util/code.hpp>
#include <util/platform.hpp>

#include <cerrno>
#include <cstring>
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
	#include <libkern/OSCacheControl.h>
	#include <pthread.h>
	#include <sys/mman.h>
#elif LI_UNIX
	#include <sys/mman.h>
#endif

namespace li::platform {
	namespace {
		std::error_code invalid_mapping_error() noexcept { return std::make_error_code(std::errc::bad_address); }

#if LI_WINDOWS
		std::error_code windows_error() noexcept {
			const DWORD value = GetLastError();
			return std::error_code(static_cast<int>(value), std::system_category());
		}
#elif LI_OSX
		void copy_to_jit_memory(void* destination, const void* source, std::size_t size) noexcept {
	#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
		#pragma clang diagnostic push
		#pragma clang diagnostic ignored "-Wunguarded-availability-new"
			if (&pthread_jit_write_protect_supported_np != nullptr && pthread_jit_write_protect_supported_np()) {
				pthread_jit_write_protect_np(0);
				std::memcpy(destination, source, size);
				pthread_jit_write_protect_np(1);
				return;
			}
		#pragma clang diagnostic pop
	#endif
			std::memcpy(destination, source, size);
		}
#endif
	};

	void code_memory::release() noexcept {
		if (address_) {
#if LI_WINDOWS
			VirtualFree(address_, 0, MEM_RELEASE);
#elif LI_OSX || LI_UNIX
			munmap(address_, allocation_size_);
#endif
		}
		address_         = nullptr;
		capacity_        = 0;
		allocation_size_ = 0;
		page_size_       = 0;
		writable_        = false;
		published_       = false;
	}

	code_memory::~code_memory() noexcept { release(); }

	code_memory::code_memory(code_memory&& other) noexcept
		 : address_(std::exchange(other.address_, nullptr)),
			capacity_(std::exchange(other.capacity_, 0)),
			allocation_size_(std::exchange(other.allocation_size_, 0)),
			page_size_(std::exchange(other.page_size_, 0)),
			writable_(std::exchange(other.writable_, false)),
			published_(std::exchange(other.published_, false)) {}

	code_memory& code_memory::operator=(code_memory&& other) noexcept {
		if (this != &other) {
			release();
			address_         = std::exchange(other.address_, nullptr);
			capacity_        = std::exchange(other.capacity_, 0);
			allocation_size_ = std::exchange(other.allocation_size_, 0);
			page_size_       = std::exchange(other.page_size_, 0);
			writable_        = std::exchange(other.writable_, false);
			published_       = std::exchange(other.published_, false);
		}
		return *this;
	}

	code_memory code_memory::allocate(std::size_t capacity, std::error_code& error) noexcept {
		error.clear();
		if (capacity == 0) [[unlikely]] {
			error = std::make_error_code(std::errc::invalid_argument);
			return {};
		}

		const std::size_t page_size = native_page_size(error);
		if (error)
			return {};

		std::size_t allocation_size = 0;
		if (!round_to_page(capacity, page_size, allocation_size)) [[unlikely]] {
			error = std::make_error_code(std::errc::value_too_large);
			return {};
		}

		void* allocation = nullptr;
#if LI_WINDOWS
		allocation = VirtualAlloc(nullptr, allocation_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (!allocation) [[unlikely]] {
			error = windows_error();
			return {};
		}
#elif LI_OSX
	#ifdef MAP_JIT
		allocation = mmap(nullptr, allocation_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
		if (allocation == MAP_FAILED) [[unlikely]] {
			error = std::error_code(errno, std::generic_category());
			return {};
		}
	#else
		error = std::make_error_code(std::errc::not_supported);
		return {};
	#endif
#elif LI_UNIX
		allocation = mmap(nullptr, allocation_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (allocation == MAP_FAILED) [[unlikely]] {
			error = std::error_code(errno, std::generic_category());
			return {};
		}
#else
		error = std::make_error_code(std::errc::not_supported);
		return {};
#endif

		code_memory result;
		result.address_         = static_cast<std::byte*>(allocation);
		result.capacity_        = capacity;
		result.allocation_size_ = allocation_size;
		result.page_size_       = page_size;
		result.writable_        = true;
		return result;
	}

	std::error_code code_memory::write(std::size_t offset, std::span<const std::byte> bytes) noexcept {
		if (!address_) [[unlikely]]
			return invalid_mapping_error();
		if (!writable_) [[unlikely]]
			return std::make_error_code(std::errc::operation_not_permitted);
		if (offset > capacity_ || bytes.size() > capacity_ - offset) [[unlikely]]
			return std::make_error_code(std::errc::result_out_of_range);
		if (bytes.empty())
			return {};

#if LI_OSX
		copy_to_jit_memory(address_ + offset, bytes.data(), bytes.size());
#else
		std::memcpy(address_ + offset, bytes.data(), bytes.size());
#endif
		published_ = false;
		return {};
	}

	std::error_code code_memory::publish() noexcept {
		if (!address_) [[unlikely]]
			return invalid_mapping_error();

		if (writable_) {
#if LI_WINDOWS
			DWORD old_protection = 0;
			if (!VirtualProtect(address_, allocation_size_, PAGE_EXECUTE_READ, &old_protection)) [[unlikely]]
				return windows_error();
#elif LI_UNIX
			if (mprotect(address_, allocation_size_, PROT_READ | PROT_EXEC) != 0) [[unlikely]]
				return std::error_code(errno, std::generic_category());
#endif
			writable_  = false;
			published_ = false;
		}

#if LI_WINDOWS
		if (!FlushInstructionCache(GetCurrentProcess(), address_, capacity_)) [[unlikely]]
			return windows_error();
#elif LI_OSX
		sys_icache_invalidate(address_, capacity_);
#elif LI_UNIX
		__builtin___clear_cache(reinterpret_cast<char*>(address_), reinterpret_cast<char*>(address_ + capacity_));
#else
		return std::make_error_code(std::errc::not_supported);
#endif
		published_ = true;
		return {};
	}

	std::error_code code_memory::reopen_for_write(inactive_code_t) noexcept {
		if (!address_) [[unlikely]]
			return invalid_mapping_error();
		if (writable_)
			return {};

#if LI_WINDOWS
		DWORD old_protection = 0;
		if (!VirtualProtect(address_, allocation_size_, PAGE_READWRITE, &old_protection)) [[unlikely]]
			return windows_error();
#elif LI_UNIX
		if (mprotect(address_, allocation_size_, PROT_READ | PROT_WRITE) != 0) [[unlikely]]
			return std::error_code(errno, std::generic_category());
#endif
		writable_  = true;
		published_ = false;
		return {};
	}
};
