#include <algorithm>
#include <fstream>
#include <lang/lexer.hpp>
#include <lang/operator.hpp>
#include <lang/parser.hpp>
#include <lib/std.hpp>
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

namespace li::debug {
	static void dump_table(table* t) {
		for (auto& [k, v] : *t) {
			if (k != none) {
				printf("%s->%s [hash=%x]\n", k.to_string().c_str(), v.to_string().c_str(), k.hash());
			}
		}
	}

	static void dump_tokens(vm* L, std::string_view s) {
		lex::state lexer{L, s};
		size_t     last_line = 0;
		while (true) {
			if (last_line != lexer.line) {
				printf("\n%03zu: ", lexer.line);
				last_line = lexer.line;
			}
			auto token = lexer.next();
			if (token == lex::token_eof)
				break;
			putchar(' ');
			token.print();
		}
		puts("");
	}
};

using namespace li;
#ifdef __EMSCRIPTEN__
static vm* emscripten_vm = nullptr;
extern "C" {
void __attribute__((used)) runscript(const char* str) {
	vm*  L  = emscripten_vm;
	auto fn = li::load_script(L, str, "console", true);
	if (fn.is(type_function)) {
		L->push_stack(fn);
		if (!L->scall(0)) {
			printf(LI_RED "Exception: ");
			L->pop_stack().print();
			printf("\n" LI_DEF);
		} else {
			auto r = L->pop_stack();
			if (!r.is(type_none)) {
				printf(LI_GRN "");
				r.print();
				printf("\n" LI_DEF);

				if (r.is(type_table))
					debug::dump_table(r.as_tbl());
			}
		}
		// puts("---------GLOBALS------------");
		// debug::dump_table(L->globals);
	} else {
		printf(LI_RED "Parser error: %s\n" LI_DEF, fn.as_str()->data);
	}
}
};
#endif

int main(int argv, const char** args) {
	platform::setup_ansi_escapes();

	
#ifndef __EMSCRIPTEN__

	// Take in a file name.
	//
	if (argv != 2) {
		printf(LI_RED "Expected li <file-name>\n");
		return 0;
	}

	// Read the file.
	//
	std::ifstream file(args[1]);
	if (!file.good()) {
		printf(LI_RED "Failed reading file '%s'\n", args[1]);
		return 1;
	}
	std::string file_buf{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};

	// Create the VM.
	//
	auto* L = vm::create();
	lib::register_std(L);

	// Execute the code, print the result.
	//
	auto fn = li::load_script(L, file_buf);

	// Validate, print the result.
	//
	if (fn.is(type_function)) {
		L->push_stack(fn);

		auto t0 = std::chrono::high_resolution_clock::now();
		if (!L->scall(0)) {
			auto t1 = std::chrono::high_resolution_clock::now();
			printf(LI_BLU "(%.2lf ms) " LI_RED "Exception: " LI_DEF, (t1 - t0) / std::chrono::duration<double, std::milli>(1.0));
			L->pop_stack().print();
			putchar('\n');
		} else {
			auto t1 = std::chrono::high_resolution_clock::now();
			printf(LI_BLU "(%.2lf ms) " LI_GRN "Result: " LI_DEF, (t1 - t0) / std::chrono::duration<double, std::milli>(1.0));
			auto r = L->pop_stack();
			r.print();
			putchar('\n');
			if (r.is(type_table))
				debug::dump_table(r.as_tbl());
		}
	} else {
		printf(" -> Parser error: %s\n", fn.as_str()->data);
	}
	L->close();
#else
	auto* emscripten_vm = vm::create();
	lib::register_std(emscripten_vm);
#endif
	return 0;
}