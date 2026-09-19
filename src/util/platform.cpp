#include <util/format.hpp>
#include <util/platform.hpp>
#include <vm/state.hpp>

#if LI_WINDOWS
	#define NOMINMAX
	#define WIN32_LEAN_AND_MEAN
	#include <Windows.h>
	#pragma comment(lib, "ntdll.lib")

extern "C" {
__declspec(dllimport) int32_t __stdcall NtAllocateVirtualMemory(
	 HANDLE ProcessHandle, PVOID* BaseAddress, ULONG_PTR ZeroBits, PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect);
__declspec(dllimport) int32_t __stdcall NtFreeVirtualMemory(HANDLE ProcessHandle, PVOID* BaseAddress, PSIZE_T RegionSize, ULONG FreeType);
};

#else
	#include <sys/mman.h>
#endif

#include <cerrno>
#include <limits>
#include <random>

#if LI_UNIX && !LI_ARCH_X86
	#include <unistd.h>
#endif

namespace li::platform {
#if !(LI_WINDOWS || LI_OSX || (LI_UNIX && LI_ARCH_X86))
	std::size_t native_page_size(std::error_code& error) noexcept {
	#if LI_UNIX
		struct page_info {
			std::size_t     size;
			std::error_code error;
		};
		static const page_info page = []() noexcept {
			errno           = 0;
			const long size = sysconf(_SC_PAGESIZE);
			if (size <= 0) [[unlikely]]
				return page_info{0, errno ? std::error_code(errno, std::generic_category()) : std::make_error_code(std::errc::not_supported)};
			return page_info{static_cast<std::size_t>(size), {}};
		}();
		if (page.error) [[unlikely]]
			error = page.error;
		return page.size;
	#else
		error = std::make_error_code(std::errc::not_supported);
		return 0;
	#endif
	}
#endif
	static bool page_count_to_size(size_t page_count, size_t& size) {
		if (page_count > (std::numeric_limits<size_t>::max() >> 12)) [[unlikely]] {
			return false;
		}
		size = page_count << 12;
		return true;
	}

	uint64_t srng() {
		std::random_device dev{};
		return li::bit_cast<uint64_t>(std::array<uint32_t, 2>{dev(), dev()});
	}

#if LI_WINDOWS
	void* page_alloc(void*, void* pointer, size_t page_count, bool executable) {
		if (pointer) {
			SIZE_T region_size = 0;
			LI_ASSERT(NtFreeVirtualMemory(HANDLE(-1), &pointer, &region_size, MEM_RELEASE) >= 0);
			return nullptr;
		} else if (page_count) {
			void*  base = nullptr;
			SIZE_T size = 0;
			if (!page_count_to_size(page_count, size)) [[unlikely]] {
				return nullptr;
			}
			if (NtAllocateVirtualMemory(HANDLE(-1), &base, 0, &size, MEM_COMMIT | MEM_RESERVE, executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE) < 0)
				 [[unlikely]] {
				return nullptr;
			}
			return base;
		} else {
			return nullptr;
		}
	}

	void setup_ansi_escapes() {
		auto console_handle = GetStdHandle(STD_OUTPUT_HANDLE);
		SetConsoleOutputCP(CP_UTF8);
		DWORD mode = 0;
		GetConsoleMode(console_handle, &mode);
		mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
		SetConsoleMode(console_handle, mode);
	}

	bool is_shift_down() { return GetAsyncKeyState(VK_SHIFT) & 0x8000; }
#else
	void* page_alloc(void*, void* pointer, size_t page_count, bool executable) {
		size_t size = 0;
		if (!page_count_to_size(page_count, size)) [[unlikely]] {
			return nullptr;
		}
		if (pointer) {
			munmap(pointer, size);
			return nullptr;
		} else if (page_count) {
			void* result = mmap(0, size, executable ? (PROT_READ | PROT_WRITE | PROT_EXEC) : (PROT_READ | PROT_WRITE), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			return result == MAP_FAILED ? nullptr : result;
		} else {
			return nullptr;
		}
	}
	void setup_ansi_escapes() {}

	/* TODO */
	bool is_shift_down() { return false; }
#endif
};
