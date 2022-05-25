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

namespace lightning::debug {
	using namespace core;
	static void print_object(any a) {
		switch (a.type()) {
			case type_none:
				printf("None");
				break;
			case type_false:
				printf("false");
				break;
			case type_true:
				printf("true");
				break;
			case type_number:
				printf("%lf", a.as_num());
				break;
			case type_array:
				printf("array @ %p", a.as_gc());
				break;
			case type_table:
				printf("table @ %p", a.as_gc());
				break;
			case type_string:
				printf("\"%s\"", a.as_str()->data);
				break;
			case type_userdata:
				printf("userdata @ %p", a.as_gc());
				break;
			case type_function:
				printf("function @ %p", a.as_gc());
				break;
			case type_nfunction:
				printf("nfunction @ %p", a.as_gc());
				break;
			case type_opaque:
				printf("opaque %p", a.as_opq().bits);
				break;
			case type_iopaque:
				printf("iopaque %p", a.as_opq().bits);
				break;
			case type_thread:
				printf("thread @ %p", a.as_gc());
				break;
		}
	}
	static void dump_table(table* t) {
		for (auto& [k, v] : *t) {
			if (k != core::none) {
				print_object(k);
				printf(" -> ");
				print_object(v);
				printf(" [hash=%x]\n", k.hash());
			}
		}
	}

	static void dump_tokens(core::vm* L, std::string_view s) {
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


using namespace lightning;

#include <unordered_map>

// TODO: Static objects with skip gc flag?
//       Weak ref type?


void export_global( core::vm* L, const char* name, core::nfunc_t f ) {
	auto nf = core::nfunction::create(L);
	nf->callback = f;
	L->globals->set(L, core::string::create(L, name), nf);
}

#include <thread>
int main() {
	platform::setup_ansi_escapes();

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

		// Create the VM, write the globals.
		//
		auto* L = core::vm::create();
		//debug::dump_tokens(L, file_buf);
		//printf("VM allocated @ %p\n", L);
		export_global(L, "sqrt", [](core::vm* L, const core::any* args, uint32_t n) {
			if (!args->is(core::type_number))
				return false;
			L->push_stack(core::any(sqrt(args->as_num())));
			return true;
		});
		export_global(L, "print", [](core::vm* L, const core::any* args, uint32_t n) {
			for (size_t i = 0; i != n; i++) {
				debug::print_object(args[i]);
				printf("\t");
			}
			printf("\n");
			return true;
		});
		export_global(L, "@printbc", [](core::vm* L, const core::any* args, uint32_t n) {
			if (n != 1 || !args->is(core::type_function)) {
				L->push_stack(core::string::create(L, "@printbc expects a single vfunction"));
				return false;
			}

			auto f = args->as_vfn();
			puts(
				 "Dumping bytecode of the function:\n"
				 "-----------------------------------------------------");
			for (size_t i = 0; i != f->length; i++) {
				f->opcode_array[i].print(i);
			}
			puts("-----------------------------------------------------");
			for (size_t i = 0; i != f->num_uval; i++) {
				printf(LI_CYN "u%llu:   " LI_DEF, i);
				debug::print_object(f->uvals()[i]);
				printf("\n");
			}
			puts("-----------------------------------------------------");
			return true;
		});

		// Execute the code, print the result.
		//
		puts("---------------------------------------\n");
		auto fn = lightning::core::load_script(L, file_buf);

		if (fn.is(core::type_function)) {
			L->push_stack(fn);

			
			auto t0 = std::chrono::high_resolution_clock::now();
			if (!L->scall(0)) {
				auto t1 = std::chrono::high_resolution_clock::now();
				printf("[took %lf ms]\n", (t1 - t0) / std::chrono::duration<double, std::milli>(1.0));
				printf(" -> Exception: ");
				debug::print_object(L->pop_stack());
				puts("");
			} else {
				auto t1 = std::chrono::high_resolution_clock::now();
				printf("[took %lf ms]\n", (t1 - t0) / std::chrono::duration<double, std::milli>(1.0));
				printf(" -> Result: ");
				auto r = L->pop_stack();
				debug::print_object(r);
				puts("");

				if (r.is(core::type_table))
					debug::dump_table(r.as_tbl());
			}
			// puts("---------GLOBALS------------");
			//debug::dump_table(L->globals);
		} else {
			printf(" -> Parser error: %s\n", fn.as_str()->data);
		}

		L->close();
	}




#if 0
	printf("%p\n", core::string::create(L, "hello"));
	printf("%p\n", core::string::create(L, "hello"));
	printf("%p\n", core::string::create(L, "hellox"));

	std::unordered_map<double, double> t1 = {};
	core::table*                       t2 = core::table::create(L);
	for (size_t i = 0; i != 521; i++) {
		double key   = rand() % 15;
		double value = rand() / 5214.0;

		t1[key] = value;
		t2->set(L, core::any(key), core::any(value));
	}

	printf("--------- t1 ----------\n");
	printf(" Capacity: %llu\n", t1.max_bucket_count());
	for (auto& [k, v] : t1)
		printf("%lf -> %lf\n", k, v);

	printf("--------- t2 ----------\n");
	printf(" Capacity: %llu\n", t2->size());
	t2->set(L, core::string::create(L, "hey"), core::any{5.0f});
	debug::dump_table(t2);

	printf("----------------------\n");

	for (auto it = L->gc_page_head.next; it != &L->gc_page_head; it = it->next) {
		printf("gc page %p\n", it);
	}

	std::ifstream           file("..\\lexer-test.li");
	std::string             file_buf{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	debug::dump_tokens(L, file_buf);
#endif
}