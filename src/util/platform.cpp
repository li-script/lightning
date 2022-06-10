#include <util/format.hpp>
#include <util/platform.hpp>
#include <vm/state.hpp>

#if LI_WINDOWS
	#define NOMINMAX
	#define WIN32_LEAN_AND_MEAN
	#include <Windows.h>
	#pragma comment(lib, "ntdll.lib")

extern "C" {
	__declspec(dllimport) int32_t __stdcall NtAllocateVirtualMemory(HANDLE ProcessHandle, PVOID* BaseAddress, ULONG_PTR ZeroBits, PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect);
	__declspec(dllimport) int32_t __stdcall NtFreeVirtualMemory(HANDLE ProcessHandle, PVOID* BaseAddress, PSIZE_T RegionSize, ULONG FreeType);
};

#else
	#include <sys/mman.h>
#endif

#include <random>
namespace li::platform {
	uint64_t srng() {
		std::random_device dev {};
		return li::bit_cast<uint64_t>(std::array<uint32_t, 2>{dev(), dev()});
	}

#if LI_WINDOWS
	void* page_alloc(void*, void* pointer, size_t page_count, bool executable) {
		if (pointer) {
			SIZE_T region_size = 0;
			LI_ASSERT(NtFreeVirtualMemory(HANDLE(-1), &pointer, &region_size, MEM_RELEASE) >= 0);
			return nullptr;
		} else if (page_count) {
			void* base = nullptr;
			SIZE_T size = page_count << 12;
			NtAllocateVirtualMemory(HANDLE(-1), &base, 0, &size, MEM_COMMIT | MEM_RESERVE, executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE);
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
		if (pointer) {
			munmap(pointer, page_count << 12);
			return nullptr;
		} else if (page_count) {
			return mmap(0, page_count << 12, executable ? (PROT_READ | PROT_WRITE | PROT_EXEC) : (PROT_READ | PROT_WRITE), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		} else {
			return nullptr;
		}
	}
	void setup_ansi_escapes() {}

	 /* TODO */
	bool is_shift_down() { return false; }
#endif
};
