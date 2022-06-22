#include <algorithm>
#include <fstream>
#include <iostream>
#include <cstring>
#include <lang/lexer.hpp>
#include <lang/operator.hpp>
#include <lang/parser.hpp>
#include <lib/std.hpp>
#include <lib/fs.hpp>
#include <optional>
#include <thread>
#include <tuple>
#include <util/llist.hpp>
#include <vector>
#include <vm/bc.hpp>
#include <vm/function.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <vm/object.hpp>

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
static void handle_repl_io(vm* L, std::string_view input) {
	auto fn = li::load_script(L, input, "console", {}, true);
	if (!fn.is_exc()) {
		if (auto r = L->call(0, fn); r.is_exc()) {
			printf(LI_RED "Exception: ");
			L->last_ex.print();
			printf("\n" LI_DEF);
		} else if (r != nil) {
			printf(LI_GRN "");
			r.print();
			printf("\n" LI_DEF);
			if (r.is_tbl())
				debug::dump_table(r.as_tbl());
		}
	} else {
		printf(LI_RED "Parser error: " LI_DEF);
		L->last_ex.print();
		putchar('\n');
	}
}


#if LI_ARCH_WASM
static vm* emscripten_vm = nullptr;
extern "C" {
	void __attribute__((used)) runscript(const char* str) {
		handle_repl_io(emscripten_vm, str);
	}
};
#endif

#include <util/user.hpp>

int main(int argv, const char** args) {
	platform::setup_ansi_escapes();

#if !LI_ARCH_WASM
	// Create the VM.
	//
	auto* L = vm::create();
	lib::register_std(L);

	// Parse the arguments.
	//
	const char* file_path = nullptr;
	for (int i = 1; i < argv; ++i) {
		if (!strcmp(args[i], "--no-gc")) {
			L->gc.suspend = true;
		} else if (!strcmp(args[i], "--jit")) {
			L->jit_all = true;
		} else if (!strcmp(args[i], "--jit-verbose")) {
			L->jit_all = true;
			L->jit_verbose = true;
		} else if (!file_path) {
			file_path = args[i];
		}
	}

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
				return 0;
			}

			// Execute and print.
			//
			handle_repl_io(L, buffer);
		}
	}

	// Read the file.
	//
	auto file = lib::fs::read_string(file_path);
	if (!file) {
		printf(LI_RED "Failed reading file '%s'\n" LI_DEF, file_path);
		return 1;
	}
	auto fn = li::load_script(L, *file, file_path);

	// Validate, print the result.
	//
	int retval = 1;
	if (!fn.is_exc()) {
		auto t0 = std::chrono::high_resolution_clock::now();
		if (auto r = L->call(0, fn); r.is_exc()) {
			auto t1 = std::chrono::high_resolution_clock::now();
			printf(LI_BLU "(%.2lf ms) " LI_RED "Exception: " LI_DEF, (t1 - t0) / std::chrono::duration<double, std::milli>(1.0));
			if (r = L->last_ex; r == nil)
				printf("?");
			else
				r.print();
			putchar('\n');
		} else {
			auto t1 = std::chrono::high_resolution_clock::now();
			printf(LI_BLU "(%.2lf ms) " LI_GRN "Result: " LI_DEF, (t1 - t0) / std::chrono::duration<double, std::milli>(1.0));
			if (r == nil)
				printf("OK");
			else
				r.print();
			putchar('\n');
			if (r.is_tbl())
				debug::dump_table(r.as_tbl());
			retval = 0;
		}
	} else {
		printf(LI_RED "Parser error: " LI_DEF);
		L->last_ex.print();
		putchar('\n');
	}
	L->close();
	return retval;
#else
	emscripten_vm = vm::create();
	lib::register_std(emscripten_vm);
	return 0;
#endif
}