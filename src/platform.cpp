#include <util/format.hpp>
#include <util/platform.hpp>
#include <vm/state.hpp>

#ifdef _WIN32
	#define NOMINMAX
	#define WIN32_LEAN_AND_MEAN
	#include <Windows.h>
#else
	#include <sys/mman.h>
#endif

namespace li::platform {
#ifdef _WIN32
	void* page_alloc(void*, void* pointer, size_t page_count, bool executable) {
		if (pointer) {
			VirtualFree(pointer, 0, MEM_RELEASE);
			return nullptr;
		} else if (page_count) {
			return VirtualAlloc(nullptr, page_count << 12, MEM_COMMIT | MEM_RESERVE, executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE);
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
#endif
};
