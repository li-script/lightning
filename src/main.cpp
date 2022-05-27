#include <fstream>
#include <lang/lexer.hpp>

#include <util/llist.hpp>
#include <vector>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>

/*
	statement == expression

	struct array;
	struct userdata;
	struct function;
	struct thread;
	struct environment;

*/

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
				printf("\n%03llu: ", lexer.line);
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

#include <algorithm>
#include <optional>
#include <tuple>

#include <vm/bc.hpp>
#include <vm/function.hpp>
#include <lang/operator.hpp>
#include <lang/parser.hpp>


using namespace li;

#include <unordered_map>
#include <thread>

// TODO: Static objects with skip gc flag?
//       Weak ref type?


void export_global( vm* L, const char* name, nfunc_t f ) {
	auto nf = nfunction::create(L);
	nf->callback = f;
	L->globals->set(L, string::create(L, name), nf);
}


static vm* create_vm_with_stdlib() {
	auto* L = vm::create();
	export_global(L, "sqrt", [](vm* L, const any* args, uint32_t n) {
		if (!args->is(type_number))
			return false;
		L->push_stack(any(sqrt(args->as_num())));
		return true;
	});
	export_global(L, "print", [](vm* L, const any* args, uint32_t n) {
		for (size_t i = 0; i != n; i++) {
			args[i].print();
			printf("\t");
		}
		printf("\n");
		return true;
	});
	export_global(L, "@gc", [](vm* L, const any* args, uint32_t n) {
		L->gc.collect(L);
		return true;
	});
	export_global(L, "@printbc", [](vm* L, const any* args, uint32_t n) {
		if (n != 1 || !args->is(type_function)) {
			L->push_stack(string::create(L, "@printbc expects a single vfunction"));
			return false;
		}

		auto f = args->as_vfn();
		puts(
			 "Dumping bytecode of the function:\n"
			 "-----------------------------------------------------");
		for (uint32_t i = 0; i != f->length; i++) {
			f->opcode_array[i].print(i);
		}
		puts("-----------------------------------------------------");
		for (uint32_t i = 0; i != f->num_uval; i++) {
			printf(LI_CYN "u%u:   " LI_DEF, i);
			f->uvals()[i].print();
			printf("\n");
		}
		puts("-----------------------------------------------------");
		return true;
	});
	return L;
}

#ifdef __EMSCRIPTEN__
static vm* emscripten_vm = nullptr;
extern "C" {
	void  __attribute__((used)) runscript(const char* str) {
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

int main() {
	platform::setup_ansi_escapes();

#ifndef __EMSCRIPTEN__
	std::string last_exec;  
	while (true) {
		// Read the file.
		//
		std::ifstream file("..\\parser-test.li");
		std::string   file_buf{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};

		// If nothing changed, sleep for 100ms.
		//
		if (last_exec == file_buf) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}
		last_exec = file_buf;

		vm* L = create_vm_with_stdlib();

		// Execute the code, print the result.
		//
		puts("---------------------------------------\n");
		auto fn = li::load_script(L, last_exec.c_str());

		if (fn.is(type_function)) {
			L->push_stack(fn);

			auto t0 = std::chrono::high_resolution_clock::now();
			if (!L->scall(0)) {
				auto t1 = std::chrono::high_resolution_clock::now();
				printf("[took %lf ms]\n", (t1 - t0) / std::chrono::duration<double, std::milli>(1.0));
				printf(" -> Exception: ");
				L->pop_stack().print();
				putchar('\n');
			} else {
				auto t1 = std::chrono::high_resolution_clock::now();
				printf("[took %lf ms]\n", (t1 - t0) / std::chrono::duration<double, std::milli>(1.0));
				printf(" -> Result: ");
				auto r = L->pop_stack();
				r.print();
				putchar('\n');

				if (r.is(type_table))
					debug::dump_table(r.as_tbl());
			}
			// puts("---------GLOBALS------------");
			// debug::dump_table(L->globals);
		} else {
			printf(" -> Parser error: %s\n", fn.as_str()->data);
		}
		L->close();
	}
#else
	emscripten_vm = create_vm_with_stdlib();
#endif
	return 0;
}