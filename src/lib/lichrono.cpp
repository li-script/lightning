#include <bit>
#include <chrono>
#include <lang/parser.hpp>
#include <lib/std.hpp>
#include <util/user.hpp>

// Include arch-specific header if relevant for optimizations.
//
#if LI_JIT && LI_ARCH_X86 && !LI_32
	#include <ir/x86-64.hpp>
#endif

namespace li::lib {
	static double chrono_cycles_c() {
		uint64_t cycles = 0;
#if __has_builtin(__builtin_readcyclecounter)
		cycles = __builtin_readcyclecounter();
#elif LI_ARCH_X86 && LI_GNU
		uint32_t low, high;
		asm("rdtsc" : "=a"(low), "=d"(high)::);
		cycles = low | (uint64_t(high) << 32);
#elif LI_ARCH_X86 && LI_MSVC
		cycles = __rdtsc();
#endif
		return (double)cycles;
	}
	static util::native_function chrono_cycles = {
		 func_attr_none,
		 "chrono.cycles",
		 [](vm* L, any* args, slot_t n) { return L->ok(chrono_cycles_c()); },
		 {{li::bit_cast<const void*>(&chrono_cycles_c), {}, type::f64}},
	};

	// Registers the chrono library.
	//
	void register_chrono(vm* L) {
		// Time.
		//
		util::export_as(L, "chrono.now", [](vm* L, any* args, slot_t n) {
			auto time = std::chrono::high_resolution_clock::now().time_since_epoch();
			return L->ok(number(time / std::chrono::duration<double, std::milli>(1)));
		});
		chrono_cycles.export_into(L);

		// Arch specific optimization.
		//
#if LI_JIT && LI_ARCH_X86 && !LI_32
		using namespace ir;
		chrono_cycles.nfi.overloads.front().mir_lifter = [](mblock& b, insn* i) {
			auto rdx = mreg(arch::from_native(zy::RDX));
			auto rax = mreg(arch::from_native(zy::RAX));
			RDTSC(b);
			SHL(b, rdx, 32);
			OR(b, rdx, rax);
			b.append(vop::fcvt, REG(i), rdx);
			return true;
		};
#endif
	}
};
