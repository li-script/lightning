#include <lib/std.hpp>
#include <util/user.hpp>
#include <lang/parser.hpp>
#include <bit>
#include <chrono>

namespace li::lib {
	// Registers the chrono library.
	//
	void register_chrono(vm* L) {
		// Time.
		//
		util::export_as(L, "chrono.now", [](vm* L, any* args, slot_t n) {
			auto time = std::chrono::high_resolution_clock::now().time_since_epoch();
			return L->ok(number(time / std::chrono::duration<double, std::milli>(1)));
		});
		util::export_as(L, "chrono.cycles", [](vm* L, any* args, slot_t n) {
			uint64_t cycles = 0;
#if __has_builtin(__builtin_readcyclecounter)
			cycles = __builtin_readcyclecounter();
#elif LI_ARCH_X86 && LI_MSVC
			cycles = __rdtsc();
#endif
			return L->ok(number(cycles));
		});
	}
};
