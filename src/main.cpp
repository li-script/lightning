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

// weak ref type?

using namespace lightning;

#include <unordered_map>

int main() {
	platform::setup_ansi_escapes();

	auto* L = core::vm::create();
	printf("VM allocated @ %p\n", L);

	#if 0
	std::vector<core::any> constants = {};
	std::vector<bc::insn>  ins;

	ins.emplace_back(bc::insn{bc::KIMM, 0}).xmm() = core::any(core::number(2)).value;
	ins.emplace_back(bc::insn{bc::AADD, -1, -1, -2});  // arg 1 = arg 1 + arg 2
	ins.emplace_back(bc::insn{bc::AMUL, -1, -1, 0});   // arg 1 = arg 1 + local 0
	ins.emplace_back(bc::insn{bc::RETN, -1});          // ret arg 1

	for (size_t i = 0; i != ins.size(); i++) {
		ins[i].print(i);
	}

	core::function* f = core::function::create(L, ins, constants, 0);
	f->num_locals     = 1;
	
	L->push_stack(f);
	L->push_stack(core::number(1));
	//	L->push_stack(core::string::format(L, "lol: %d", 4));
	L->push_stack(core::number(2));

	if (bool is_ex = core::vm_enter(L, 2)) {
		printf("Execution finished with exception: ");
		debug::print_object(L->pop_stack());
		puts("");
	} else {
		printf("Execution finished with result: ");
		debug::print_object(L->pop_stack());
		puts("");
	}
	#endif

	{
		std::ifstream file("..\\parser-test.li");
		std::string   file_buf{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
		debug::dump_tokens(L, file_buf);

		puts("---------------------------------------\n");
		auto fn = lightning::core::load_script(L, file_buf);
		if (fn.is(core::type_function)) {
			L->push_stack(fn);

			if (!L->scall(0)) {
				printf("Execution finished with exception: ");
				debug::print_object(L->pop_stack());
				puts("");
			} else {
				printf("Execution finished with result: ");
				auto r = L->pop_stack();
				debug::print_object(r);
				puts("");

				if (r.is(core::type_table))
					debug::dump_table(r.as_tbl());
			}
			puts("---------GLOBALS------------");

			debug::dump_table(L->globals);
		} else {
			printf("Parsing failed with error: %s\n", fn.as_str()->data);
		}

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