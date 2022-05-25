#include <vm/state.hpp>
#include <util/format.hpp>
#include <util/platform.hpp>

#ifdef _WIN64
	#define NOMINMAX
	#define WIN32_LEAN_AND_MEAN
	#include <Windows.h>
#else
	#include <sys/mman.h>
#endif

namespace lightning::platform {
#ifdef _WIN64
	void* page_alloc(core::vm*, void* pointer, size_t page_count, bool executable) {
		if (pointer) {
			VirtualFree(pointer, 0, MEM_RELEASE);
			return nullptr;
		} else {
			return VirtualAlloc(nullptr, page_count << 12, MEM_COMMIT | MEM_RESERVE, executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE);
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
	void* page_alloc(core::vm*, void* pointer, size_t page_count, bool executable) {
		if (pointer) {
			munmap(pointer, page_count << 12);
			return nullptr;
		} else {
			return mmap(0, page_count << 12, executable ? (PROT_READ | PROT_WRITE | PROT_EXEC) : (PROT_READ | PROT_WRITE), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		}
	}
	void setup_ansi_escapes() {}
#endif
};
