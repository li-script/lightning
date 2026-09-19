#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <lang/lexer.hpp>
#include <lang/operator.hpp>
#include <lang/parser.hpp>
#include <lib/fs.hpp>
#include <lib/std.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <util/llist.hpp>
#include <vector>
#include <vm/bc.hpp>
#include <vm/function.hpp>
#include <vm/object.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>

namespace li::debug {
	static void dump_table(table* t) {
		for (auto& [k, v] : *t) {
			if (k != nil) {
				printf("%s->%s [hash=%zx]\n", k.to_string().c_str(), v.to_string().c_str(), k.hash());
			}
		}
	}
};

using namespace li;

namespace {
#if !LI_ARCH_WASM
	constexpr uint64_t max_benchmark_runs = 1'000'000;

	uint64_t elapsed_ns(std::chrono::steady_clock::time_point start) {
		auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
		return uint64_t(elapsed.count());
	}

	void print_usage(const char* program) {
		fprintf(stderr, "Usage: %s [--jit|--jit-verbose|--jit=auto|--jit=off] [--metrics] [--verbose-errors] [--benchmark-runs=N] [--] [script]\n",
			 program ? program : "li");
	}

	struct execution_metrics {
		uint64_t                parse_compile_ns = 0;
		std::optional<uint64_t> first_call_ns{};
		std::vector<uint64_t>   warm_call_ns{};
		uint64_t                allocations = 0;
		uint64_t                retains     = 0;
		uint64_t                releases    = 0;
	};

	void emit_metrics(const vm* L, const execution_metrics& metrics) {
		std::string json = "Metrics: {\"parse_compile_ns\":" + std::to_string(metrics.parse_compile_ns) +
								 ",\"jit_compile_ns\":" + std::to_string(L->jit_compile_ns) + ",\"first_call_ns\":";
		if (metrics.first_call_ns)
			json += std::to_string(*metrics.first_call_ns);
		else
			json += "null";
		json += ",\"warm_call_ns\":[";
		for (size_t i = 0; i != metrics.warm_call_ns.size(); ++i) {
			if (i)
				json += ',';
			json += std::to_string(metrics.warm_call_ns[i]);
		}
		json += "],\"generated_code_bytes\":" + std::to_string(L->generated_code_bytes) + ",\"compiled_functions\":" + std::to_string(L->jit_compiled) +
				  ",\"allocations\":" + std::to_string(metrics.allocations) + ",\"retains\":" + std::to_string(metrics.retains) +
				  ",\"releases\":" + std::to_string(metrics.releases) + ",\"spill_slots\":" + std::to_string(L->jit_spill_slots) +
				  ",\"jit_guards\":" + std::to_string(L->jit_guards) + "}\n";
		fputs(json.c_str(), stderr);
	}
#endif
}

static void handle_repl_io(vm* L, std::string_view input) {
	auto fn = li::load_script(L, input, "console", {}, true);
	if (!fn.is_exc()) {
		if (auto r = L->call(0, fn); r.is_exc()) {
			printf(LI_RED "Exception: ");
			L->last_ex.print();
			printf("\n" LI_DEF);
			if (auto trace = L->format_exception_trace(); !trace.empty())
				fputs(trace.c_str(), stdout);
		} else {
			if (r != nil) {
				printf(LI_GRN "");
				r.print();
				printf("\n" LI_DEF);
				if (r.is_tbl())
					debug::dump_table(r.as_tbl());
			}
			rc::release(L, r);
		}
		rc::release(L, fn);
	} else {
		printf(LI_RED "Parser error: " LI_DEF);
		L->last_ex.print();
		putchar('\n');
	}
	fflush(stdout);
	fflush(stderr);
}

#if LI_ARCH_WASM
static vm* emscripten_vm = nullptr;

static vm* get_or_create_wasm_vm() {
	if (!emscripten_vm) {
		emscripten_vm = vm::create();
		if (emscripten_vm) {
			lib::register_std(emscripten_vm);
		}
	}
	return emscripten_vm;
}

extern "C" {
void __attribute__((used)) runscript(const char* str) {
	if (auto* L = get_or_create_wasm_vm()) {
		handle_repl_io(L, str);
	}
}

void __attribute__((used)) reset_vm() {
	if (emscripten_vm) {
		emscripten_vm->close();
		emscripten_vm = nullptr;
	}
	get_or_create_wasm_vm();
}
};
#endif

#include <util/user.hpp>

#if LI_ARCH_WASM
int main() {
#else
int main(int argv, const char** args) {
#endif
	platform::setup_ansi_escapes();

#if !LI_ARCH_WASM
	// Parse arguments that affect VM creation and script loading.
	//
	const char* file_path = nullptr;
	#if LI_JIT
	tier::mode requested_jit = tier::mode::automatic;
	#else
	tier::mode requested_jit = tier::mode::off;
	#endif
	uint64_t benchmark_runs        = 1;
	bool     requested_jit_verbose = false;
	bool     jit_option_specified  = false;
	bool     metrics_enabled       = false;
	bool     verbose_errors        = false;
	bool     benchmark_requested   = false;
	bool     positional_only       = false;
	for (int i = 1; i < argv; ++i) {
		std::string_view arg = args[i];
		if (!positional_only && arg == "--") {
			positional_only = true;
		} else if (!positional_only && arg == "--jit=auto") {
			requested_jit        = tier::mode::automatic;
			jit_option_specified = true;
		} else if (!positional_only && arg == "--jit=off") {
			requested_jit        = tier::mode::off;
			jit_option_specified = true;
		} else if (!positional_only && arg == "--jit") {
			requested_jit        = tier::mode::required;
			jit_option_specified = true;
		} else if (!positional_only && arg == "--jit-verbose") {
			requested_jit         = tier::mode::required;
			requested_jit_verbose = true;
			jit_option_specified  = true;
		} else if (!positional_only && arg == "--metrics") {
			metrics_enabled = true;
		} else if (!positional_only && arg == "--verbose-errors") {
			verbose_errors = true;
		} else if (!positional_only && arg.starts_with("--benchmark-runs=")) {
			constexpr std::string_view prefix = "--benchmark-runs=";
			std::string_view           value  = arg.substr(prefix.size());
			uint64_t                   parsed = 0;
			auto [end, error]                 = std::from_chars(value.data(), value.data() + value.size(), parsed);
			if (error != std::errc{} || end != value.data() + value.size() || parsed == 0 || parsed > max_benchmark_runs) {
				fprintf(stderr, "Invalid --benchmark-runs value '%.*s': expected an integer from 1 to %llu.\n", int(value.size()), value.data(),
					 static_cast<unsigned long long>(max_benchmark_runs));
				print_usage(args[0]);
				return 1;
			}
			benchmark_runs      = parsed;
			benchmark_requested = true;
		} else if (!positional_only && arg.starts_with("-")) {
			fprintf(stderr, "Unknown option '%s'.\n", args[i]);
			print_usage(args[0]);
			return 1;
		} else if (!file_path) {
			file_path = args[i];
		} else {
			fprintf(stderr, "Unexpected extra script path '%s'.\n", args[i]);
			print_usage(args[0]);
			return 1;
		}
	}
	if (!file_path && (metrics_enabled || benchmark_requested)) {
		fprintf(stderr, "--metrics and --benchmark-runs require a script path.\n");
		print_usage(args[0]);
		return 1;
	}

	#if !LI_JIT
	if (requested_jit == tier::mode::required) {
		fprintf(stderr, "JIT unavailable: this build has no native JIT backend.\n");
		return 1;
	}
	#endif

	// Create the VM.
	//
	auto* L = vm::create();
	if (!L) {
		fprintf(stderr, "Failed creating VM.\n");
		return 1;
	}
	L->verbose_errors = verbose_errors;
	lib::register_std(L);
	#if LI_JIT
	L->jit_policy.execution = requested_jit;
	L->jit_policy.verbose   = requested_jit_verbose;
	#else
	L->jit_policy.execution = tier::mode::off;
	#endif

	// Repl if no file given.
	//
	if (!file_path) {
		// clang-format off
		static constexpr const char hdr[] =
		LI_YLW "                 @          " LI_CYN "                                          \n"
		LI_YLW "               @@           " LI_CYN "                                          \n"
		LI_YLW "            ,@@@            " LI_CYN "   _      _  _____           _       _    \n" 
		LI_YLW "          @@@@@             " LI_CYN "  | |    (_)/ ____|         (_)     | |   \n" 
		LI_YLW "       ,@@@@@@              " LI_CYN "  | |     _| (___   ___ _ __ _ _ __ | |_  \n" 
		LI_YLW "     @@@@@@@@               " LI_CYN "  | |    | |\\___ \\ / __| '__| | '_ \\| __| \n" 
		LI_YLW "  ,@@@@@@@@@@@@@@@@@@@@@@@  " LI_CYN "  | |____| |____) | (__| |  | | |_) | |_  \n" 
		LI_YLW "               @@@@@@@@,    " LI_CYN "  |______|_|_____/ \\___|_|  |_| .__/ \\__| \n" 
		LI_YLW "              @@@@@@@       " LI_CYN "                              | |         \n" 
		LI_YLW "             @@@@@,         " LI_CYN "                              |_|         \n"
		LI_YLW "             @@@            " LI_CYN "                                          \n"
		LI_YLW "            @,              " LI_CYN "                                          \n" LI_DEF;
		// clang-format on
		puts(hdr);

		while (true) {
			std::string buffer;
			fputs(LI_BRG "> " LI_DEF, stdout);
			std::getline(std::cin, buffer);

			// While shift is being held, allow multiple lines to be inputted.
			//
			while (platform::is_shift_down()) {
				std::string buffer2;
				fputs("  ", stdout);
				std::getline(std::cin, buffer2);
				buffer += "\n";
				buffer += buffer2;
			}

			// Exit on EOF (CTRL+D).
			//
			if (std::cin.eof()) {
				L->close();
				return 0;
			}

			// Execute and print.
			//
			handle_repl_io(L, buffer);
		}
	}

	// Read the file.
	//
	execution_metrics metrics{};
	auto              file = lib::fs::read_string(file_path);
	if (!file) {
		printf(LI_RED "Failed reading file '%s'\n" LI_DEF, file_path);
		if (metrics_enabled)
			emit_metrics(L, metrics);
		L->close();
		return 1;
	}

	auto parse_start         = std::chrono::steady_clock::now();
	auto fn                  = li::load_script(L, *file, file_path);
	metrics.parse_compile_ns = elapsed_ns(parse_start);

	// Required JIT execution must have emitted native code during parsing.
	//
	int retval = 1;
	if (!fn.is_exc()) {
	#if LI_JIT
		if (requested_jit == tier::mode::required && L->jit_compiled == 0) {
			fprintf(stderr, "JIT compilation failed: no functions were compiled to native code.\n");
			rc::release(L, fn);
			if (metrics_enabled)
				emit_metrics(L, metrics);
			L->close();
			return 1;
		}
	#else
		if (jit_option_specified && requested_jit == tier::mode::automatic)
			printf("JIT unavailable; using interpreter.\n");
	#endif

		const uint64_t allocations_before = L->gc.allocations;
		const auto     rc_before          = rc::counts();
		metrics.warm_call_ns.reserve(size_t(benchmark_runs - 1));
		bool completed = true;
		for (uint64_t run = 0; run != benchmark_runs; ++run) {
			auto call_start = std::chrono::steady_clock::now();
			auto result     = L->call(0, fn);
			auto call_ns    = elapsed_ns(call_start);
			if (run == 0)
				metrics.first_call_ns = call_ns;
			else
				metrics.warm_call_ns.push_back(call_ns);

			if (result.is_exc()) {
				printf(LI_BLU "(%.2lf ms) " LI_RED "Exception: " LI_DEF, double(call_ns) / 1'000'000.0);
				if (result = L->last_ex; result == nil)
					printf("?");
				else
					result.print();
				putchar('\n');
				if (auto trace = L->format_exception_trace(); !trace.empty())
					fputs(trace.c_str(), stdout);
				completed = false;
				break;
			}

			if (run + 1 == benchmark_runs) {
				printf(LI_BLU "(%.2lf ms) " LI_GRN "Result: " LI_DEF, double(call_ns) / 1'000'000.0);
				if (result == nil)
					printf("OK");
				else
					result.print();
				putchar('\n');
				if (result.is_tbl())
					debug::dump_table(result.as_tbl());
			}
			rc::release(L, result);
		}
		const auto rc_after = rc::counts();
		metrics.allocations = L->gc.allocations - allocations_before;
		metrics.retains     = rc_after.retains - rc_before.retains;
		metrics.releases    = rc_after.releases - rc_before.releases;
		if (completed)
			retval = 0;
	#if LI_JIT
		if (jit_option_specified && requested_jit != tier::mode::off)
			printf("JIT compiled functions: %llu\n", static_cast<unsigned long long>(L->jit_compiled));
	#endif
		rc::release(L, fn);
	} else {
		printf(LI_RED "Parser error: " LI_DEF);
		L->last_ex.print();
		putchar('\n');
	}
	if (metrics_enabled)
		emit_metrics(L, metrics);
	L->close();
	return retval;
#else
	get_or_create_wasm_vm();
	return 0;
#endif
}