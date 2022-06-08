#include <lib/std.hpp>
#include <util/user.hpp>
#include <vm/function.hpp>
#include <vm/array.hpp>
#include <lang/parser.hpp>
#include <cmath>
#include <bit>
#include <chrono>
#include <numbers>

namespace li::lib {
#define OPTIONAL_SELF() \
	if (args[1] != none) {  \
		args++;              \
		n++;                 \
	}
#define REMAP_MATH_UNARY(name)                                                     \
	util::export_as(L, "math." LI_STRINGIFY(name), [](vm* L, any* args, slot_t n) { \
		if (n != 1 || !args->is_num()) {                                             \
			return L->error("expected number");                 \
		}                                                                            \
		return L->ok(any(name(args->as_num())));                                    \
	});

#define REMAP_MATH_BINARY(name)                                                    \
	util::export_as(L, "math." LI_STRINGIFY(name), [](vm* L, any* args, slot_t n) { \
		if (n != 2 || !args[0].is_num() || !args[-1].is_num()) {                     \
			return L->error("expected two numbers");            \
		}                                                                            \
		return L->ok(any(name(args[0].as_num(), args[-1].as_num())));               \
	});

	static double rad(double x) { return x * (180 / std::numbers::pi); }
	static double deg(double x) { return x * (std::numbers::pi / 180); }


	static bool math_random_to_dbl(vm* L, any* args, slot_t n, uint64_t v) {
		constexpr uint32_t mantissa_bits = 52;
		constexpr uint32_t exponent_bits = 11;
		constexpr uint32_t exponent_0    = uint32_t((1u << (exponent_bits - 1)) - 3);  // 2^-2, max at 0.499999.
		
		// Find the lowest bit set, 0 requires 1 bit to be set to '0', 1 requires 
		// 2 bits to be set to '0', each increment has half the chance of being 
		// returned compared to the previous one which leads to equal distribution 
		// between exponential levels.
		//
		constexpr uint32_t exponent_seed_bits = 64 - (mantissa_bits + 1);
		uint32_t           exponent           = exponent_0 - std::countr_zero<uint32_t>(uint32_t(v) | (1u << exponent_seed_bits));

		// Clear and replace the exponent bits.
		//
		v &= ~((1ull << exponent_bits) - 1);
		v |= exponent;

		// Rotate until mantissa is at the bottom, add 0.5 to shift from [-0.5, +0.5] to [0.0, 1.0].
		//
		v = std::rotl( v, mantissa_bits );
		double r = bit_cast<double>(v) + 0.5;

		// If no args given, return.
		//
		if (n == 0) {
			return L->ok(r);
		}

		// Validate args.
		//
		if (n != 1 && n != 2) {
			return L->error("expected one or two numbers.");
		}

		// If one arg given, generate [0, x] inclusive.
		//
		if (!args[1-n].is_num())
			return L->error("expected one or two numbers.");
		double x = args[1-n].as_num();

		// If two args given, generate [y, x] inclusive.
		//
		double y = 0;
		if (n == 2) {
			if (!args[0].is_num())
				return L->error("expected one or two numbers.");
			y = args[0].as_num();
		}
		return L->ok(y + r * (x - y));
	}

	static bool math_random(vm* L, any* args, slot_t n) { return math_random_to_dbl(L, args, n, L->random()); }
	static bool math_srandom(vm* L, any* args, slot_t n) { return math_random_to_dbl(L, args, n, platform::srng()); }

	// Registers standard library.
	//
	void register_std(vm* L) {
		// Math library.
		//
		util::export_as(L, "math.fast", bool(LI_FAST_MATH));
		util::export_as(L, "math.random", math_random);
		util::export_as(L, "math.srandom", math_srandom);
		util::export_as(L, "math.eps", std::numeric_limits<double>::epsilon());
		util::export_as(L, "math.inf", std::numeric_limits<double>::infinity());
		util::export_as(L, "math.nan", std::numeric_limits<double>::quiet_NaN());
		util::export_as(L, "math.pi", std::numbers::pi);
		util::export_as(L, "math.e",  std::numbers::e);

		static constexpr auto max = [](double a, double b) { return fmax(a, b); };
		static constexpr auto min = [](double a, double b) { return fmin(a, b); };
		static constexpr auto abs = [](double a) { return fabs(a); };
		static constexpr auto sgn = [](double a) -> double { return a < 0 ? -1 : +1; };

		REMAP_MATH_BINARY(min);
		REMAP_MATH_BINARY(copysign);
		REMAP_MATH_BINARY(max);
		REMAP_MATH_BINARY(atan2);
		REMAP_MATH_UNARY(deg);
		REMAP_MATH_UNARY(rad);
		REMAP_MATH_UNARY(sqrt);
		REMAP_MATH_UNARY(abs);
		REMAP_MATH_UNARY(cos);
		REMAP_MATH_UNARY(sin);
		REMAP_MATH_UNARY(tan);
		REMAP_MATH_UNARY(sgn);
		REMAP_MATH_UNARY(round);
		REMAP_MATH_UNARY(ceil);
		REMAP_MATH_UNARY(floor);
		REMAP_MATH_UNARY(acos);
		REMAP_MATH_UNARY(asin);
		REMAP_MATH_UNARY(atan);
		REMAP_MATH_UNARY(log);
		REMAP_MATH_UNARY(log10);

		// Protected call.
		//
		util::export_as(L, "pcall", [](vm* L, any* args, slot_t n) {
			vm_guard _g{L, args};
			if (n < 2) {
				return L->error("expected 2 or more arguments.");
			}
			auto f = args[0];
			if (!f.is_fn()) {
				args[-1] = any(false);
				return L->error("invoking non-function");
			}

			auto apos = &args[0] - L->stack;
			for (slot_t i = -(n - 1); i < -1; i++)
				L->push_stack(L->stack[apos+i]);

			bool res           = L->call(n - 2, f);
			L->stack[apos - 1] = res;
			return L->ok(L->pop_stack());
		});

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

		// Debug.
		//
		util::export_as(L, "debug.stacktrace", [](vm* L, any* args, slot_t n) {
			vm_guard _g{L, args};

			auto result = array::create(L, 0, 10);
			auto cstr   = string::create(L, "C");
			auto lstr   = string::create(L, "line");
			auto fstr   = string::create(L, "func");

			call_frame frame = L->last_vm_caller;
			while (frame.stack_pos >= FRAME_SIZE) {
				auto& target = L->stack[frame.stack_pos + FRAME_TARGET];

				if (frame.multiplexed_by_c()) {
					auto tbl = table::create(L, 1);
					tbl->set(L, fstr, cstr);
					result->push(L, tbl);
				}

				auto tbl = table::create(L, 2);
				if (target.is_fn() && target.as_fn()->is_virtual()) {
					tbl->set(L, lstr, any(number(target.as_fn()->proto->lookup_line(frame.caller_pc & ~FRAME_C_FLAG))));
				}
				tbl->set(L, fstr, any(target));
				result->push(L, tbl);

				auto ref = L->stack[frame.stack_pos + FRAME_CALLER];
				if (ref.is_opq()) {
					frame = bit_cast<call_frame>(ref.as_opq());
				} else {
					break;
				}
			}
			return L->ok(result);
		});
		util::export_as(L, "debug.getuval", [](vm* L, any* args, slot_t n) {
			if (n != 2) {
				return L->error("expected 2 arguments.");
			}
			auto f = args[0];
			if (!f.is_fn()) {
				return L->error("expected function.");
			}

			auto i = args[-1];
			if (!i.is_num() || i.as_num() < 0) {
				return L->error("expected positive index.");
			}
			size_t idx = size_t(i.as_num());

			if (f.as_fn()->num_uval > idx) {
				return L->ok(f.as_fn()->uvals()[idx]);
			} else {
				return L->ok(none);
			}
		});
		util::export_as(L, "debug.setuval", [](vm* L, any* args, slot_t n) {
			if (n != 3) {
				return L->error("expected 3 arguments.");
			}
			auto f = args[0];
			if (!f.is_fn()) {
				return L->error("expected function.");
			}

			auto i = args[-1];
			if (!i.is_num() || i.as_num() < 0) {
				return L->error("expected positive index.");
			}
			size_t idx = size_t(i.as_num());

			auto u = args[-2];
			if (f.as_fn()->num_uval > idx) {
				f.as_fn()->uvals()[idx] = u;
				return L->ok(true);
			} else {
				return L->ok(false);
			}
		});
		util::export_as(L, "debug.gc", [](vm* L, any* args, slot_t n) {
			L->gc.collect(L);
			return L->ok();
		});
		util::export_as(L, "debug.dump", [](vm* L, any* args, slot_t n) {
			if (n != 1 || !args->is_fn() || !args->as_fn()->is_virtual()) {
				return L->error("dump expects a single vfunction");
			}

			auto f = args->as_fn();
			puts(
				 "Dumping bytecode of the function:\n"
				 "-----------------------------------------------------");
			msize_t last_line = 0;
			for (msize_t i = 0; i != f->proto->length; i++) {
				if (msize_t l = f->proto->lookup_line(i); l != last_line) {
					last_line = l;
					printf("ln%-50u", l);
					printf("|\n");
				}
				f->proto->opcode_array[i].print(i);
			}
			puts("-----------------------------------------------------");
			for (msize_t i = 0; i != f->num_uval; i++) {
				printf(LI_CYN "u%u:   " LI_DEF, i);
				f->uvals()[i].print();
				printf("\n");
			}
			puts("-----------------------------------------------------");
			return L->ok();
		});

		// String.
		//
		util::export_as(L, "print", [](vm* L, any* args, slot_t n) {
			for (int32_t i = 0; i != n; i++) {
				if (args[-i].is_traitful() && ((traitful_node<>*)args[-i].as_gc())->has_trait<trait::str>()) [[unlikely]] {
					fputs(args[-i].to_string(L)->c_str(), stdout);
				} else {
					args[-i].print();
				}
				printf("\t");
			}
			printf("\n");
			return L->ok();
		});
		util::export_as(L, "loadstring", [](vm* L, any* args, slot_t n) {
			if (n != 1 || !args->is_str()) {
				return L->error("expected string");
			}
			auto res = load_script(L, args->as_str()->view());
			if (!res.is_fn())
				return L->error(res);
			else
				return L->ok(res);
		});
		util::export_as(L, "eval", [](vm* L, any* args, slot_t n) {
			vm_guard _g{L, args};
			if (n != 1 || !args->is_str()) {
				return L->error("expected string");
			}
			auto res = load_script(L, args->as_str()->view());
			if (!res.is_fn()) {
				return L->error(res);
			}
			if (L->call(0, res))
				return L->ok(L->pop_stack());
			else
				return L->error(L->pop_stack());
		});

		// Misc.
		//
		util::export_as(L, "@table", [](vm* L, any* args, slot_t n) {
			uint16_t r = 0;
			if (n && args->is_num()) {
				r = (uint16_t) (uint64_t) std::abs(args->as_num());
			}
			return L->ok(table::create(L, r));
		});
		util::export_as(L, "assert", [](vm* L, any* args, slot_t n) {
			if (!n || args->coerce_bool())
				return L->ok();

			if (n >= 2 && args[-1].is_str()) {
				return L->error(args[-1]);
			} else {
				call_frame frame  = L->last_vm_caller;
				const char* fn    = "C";
				msize_t     line   = 0;
				if (frame.stack_pos >= FRAME_SIZE) {
					auto&    target = L->stack[frame.stack_pos + FRAME_TARGET];
					if (target.is_fn() && target.as_fn()->is_virtual()) {
						auto vfn = target.as_fn();
						fn       = vfn->proto->src_chunk->c_str();
						line     = vfn->proto->lookup_line(frame.caller_pc & ~FRAME_C_FLAG);
					}
				}
				return L->error("assertion failed at %s, line %u", fn, line);
			}
		});
	}
};
