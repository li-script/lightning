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

// TODO: Static objects with skip gc flag?
//       Weak ref type?


void export_global( vm* L, const char* name, nfunc_t f ) {
	auto nf = nfunction::create(L);
	nf->callback = f;
	L->globals->set(L, string::create(L, name), nf);
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
		auto* L = vm::create();
		//debug::dump_tokens(L, file_buf);
		//printf("VM allocated @ %p\n", L);
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

		// Execute the code, print the result.
		//
		puts("---------------------------------------\n");
		auto fn = li::load_script(L, file_buf);

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
			//debug::dump_table(L->globals);
		} else {
			printf(" -> Parser error: %s\n", fn.as_str()->data);
		}

		L->gc.collect(L);

		L->close();
	}




#if 0
	printf("%p\n", string::create(L, "hello"));
	printf("%p\n", string::create(L, "hello"));
	printf("%p\n", string::create(L, "hellox"));

	std::unordered_map<double, double> t1 = {};
	table*                       t2 = table::create(L);
	for (size_t i = 0; i != 521; i++) {
		double key   = rand() % 15;
		double value = rand() / 5214.0;

		t1[key] = value;
		t2->set(L, any(key), any(value));
	}

	printf("--------- t1 ----------\n");
	printf(" Capacity: %llu\n", t1.max_bucket_count());
	for (auto& [k, v] : t1)
		printf("%lf -> %lf\n", k, v);

	printf("--------- t2 ----------\n");
	printf(" Capacity: %llu\n", t2->size());
	t2->set(L, string::create(L, "hey"), any{5.0f});
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