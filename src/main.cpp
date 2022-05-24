#include <fstream>
#include <lang/lexer.hpp>

#include <util/llist.hpp>

/*
	statement == expression
*/

namespace lightning::core {


	//struct function : gc_node<function> {
	//
	//};


};

/*
	struct array;
	struct userdata;
	struct function;
	struct thread;
*/


namespace lightning::parser {

	enum class expression {

	};

	static void parse() {}

};

#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>

namespace lightning::core {
	static void print_object(any a) {
		switch (a.type) {
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
				printf("%lf", a.n);
				break;
			case type_integer:
				printf("%lld", a.i);
				break;
			case type_vec2:
				printf("<%f, %f>", a.v2.x, a.v2.y);
				break;
			case type_vec3:
				printf("<%f, %f, %f>", a.v2.x, a.v2.y, a.v3.z);
				break;
			case type_array:
				printf("array @ %p", a.a);
				break;
			case type_table:
				printf("table @ %p", a.t);
				break;
			case type_string:
				printf("\"%s\"", a.s->data);
				break;
			case type_userdata:
				printf("userdata @ %p", a.u);
				break;
			case type_function:
				printf("function @ %p", a.u);
				break;
			case type_thread:
				printf("thread @ %p", a.v);
				break;
		}
	}
	static void dump_table(table* t) {
		for (auto& [k, v] : *t) {
			if (k.type != core::type_none) {
				core::print_object(k);
				printf(" -> ");
				core::print_object(v);
				printf(" [hash=%x]\n", k.hash());
			}
		}
	}
};

// weak ref type?

using namespace lightning;

#include <unordered_map>

int main() {
	platform::setup_ansi_escapes();


	//printf("VM allocated @ %p\n", L);
	//
	//printf("%p\n", core::string::create(L, "hello"));
	//printf("%p\n", core::string::create(L, "hello"));
	//printf("%p\n", core::string::create(L, "hellox"));
	//
	//// String interning consider literals:
	////
	//
	//std::unordered_map<int64_t, double> t1 = {};
	//core::table*                        t2 = core::table::create(L);
	//for (size_t i = 0; i != 521; i++) {
	//	int64_t key   = rand() % 15;
	//	double  value = rand() / 5214.0;
	//
	//	t1[key] = value;
	//	t2->set(L, core::any(key), core::any(value));
	//}
	//
	//printf("--------- t1 ----------\n");
	//printf(" Capacity: %llu\n", t1.max_bucket_count());
	//for (auto& [k, v] : t1)
	//	printf("%lld -> %lf\n", k, v);
	//
	//printf("--------- t2 ----------\n");
	//printf(" Capacity: %llu\n", t2->size());
	//t2->set(L, core::string::create(L, "hey"), core::any{5.0f});
	//core::dump_table(t2);
	//
	//printf("----------------------\n");
	//
	//for (auto it = L->gc_page_head.next; it != &L->gc_page_head; it = it->next) {
	//	printf("gc page %p\n", it);
	//}

	std::ifstream file("S:\\Projects\\Lightning\\lexer-test.li");
	lightning::lexer::state lexer{std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}};
	
	size_t last_line = 0;
	while (true) {
	if (last_line != lexer.line) {
		printf("\n%03llu: ", lexer.line);
		last_line = lexer.line;
	}
	auto token = lexer.next();
	if (token == lexer::token_eof)
		break;
	putchar(' ');
	token.print();
	}
}