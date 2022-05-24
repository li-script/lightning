#include <fstream>
#include <lang/lexer.hpp>

#include <util/llist.hpp>

/*
	statement == expression
*/

namespace lightning::parser {

	enum class expression {

	};

	static void parse() {}

};

#include <vm/state.hpp>
#include <vm/table.hpp>


namespace lightning::core {

	struct string : gc_leaf<string> {
		static string* create(vm* L, std::string_view from);

		uint32_t   length;
		char       data[];  // Null terminated.
	};

	static void print_object( any a ) {
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

	static void dump_table( table* t ) {
		for (auto& [k,v] : *t) {
			if (k.type != core::type_none) {
				core::print_object(k);
				printf(" -> ");
				core::print_object(v);
				printf(" [hash=%x]\n", k.hash());
			}
		}
	}

	string* string::create(vm* L, std::string_view from) { 
		size_t hash = std::hash<std::string_view>{}(from); 
		auto entry = L->str_intern->get(L, any(integer(hash)));
		if (entry.type == type_none) {
			string* str = L->alloc<string>(from.size() + 1);
			memcpy(str->data, from.data(), from.size());
			str->data[from.size()] = 0;
			str->length            = (uint32_t) from.size();
			entry                  = any(str);
			L->str_intern->set(L, any(integer(hash)), entry);
		} else {
			LI_ASSERT_MSG("string hasher is too weak", 
								entry.s->length == from.length() && !memcmp(entry.s->data, from.data(), from.length()));
		}
		return entry.s;
	}

	vm* vm::create(fn_alloc a, size_t context_space) {
		// Allocate the first page.
		//
		size_t length = (std::max(minimum_gc_allocation, sizeof(vm) + context_space) + 0xFFF) >> 12;
		auto*  ptr    = a(nullptr, nullptr, length, false);
		if (!ptr)
			return nullptr;

		// Initialize the GC page, create the VM.
		//
		auto* gc    = new (ptr) gc_page(length);
		vm*   L         = gc->create<vm>(context_space);
		L->alloc_fn = a;
		util::link_after(&L->gc_page_head, gc);
		L->str_intern = table::create(L, 512);
		return L;
	}



	//string* create(vm* L, std::string_view from) {
	//
	//}


};

// weak ref type?

using namespace lightning;

#include <unordered_map>

int main() {
	util::platform_setup_ansi_escapes();

	auto* L = core::vm::create();
	printf("VM allocated @ %p\n", L);



	// String interning consider literals:
	//


	std::unordered_map<int64_t, double> t1 = {};
	core::table*                       t2 = core::table::create(L);
	for (size_t i = 0; i != 521; i++) {
		int64_t key = rand() % 15;
		double  value = rand() / 5214.0;

		t1[key] = value;
		t2->set(L, core::any(key), core::any(value));
	}

	printf("--------- t1 ----------\n");
	printf(" Capacity: %llu\n", t1.max_bucket_count());
	for (auto& [k, v] : t1)
		printf("%lld -> %lf\n", k, v);

	printf("--------- t2 ----------\n");
	printf(" Capacity: %llu\n", t2->size());
	t2->set(L, core::string::create(L, "hey"), core::any{5.0f});
	core::dump_table(t2);

	printf("----------------------\n");

	for (auto it = L->gc_page_head.next; it != &L->gc_page_head; it = it->next) {
		printf("gc page %p\n", it);
	}

	// std::ifstream file("S:\\Projects\\Lightning\\lexer-test.li");
	// lightning::lexer::state lexer{std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}};
	//
	// size_t last_line = 0;
	// while (true) {
	//	if (last_line != lexer.line) {
	//		printf("\n%03llu: ", lexer.line);
	//		last_line = lexer.line;
	//	}
	//	auto token = lexer.next();
	//	if (token == lexer::token_eof)
	//		break;
	//	putchar(' ');
	//	token.print();
	// }
}