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
namespace lightning::core {
	vm* vm::create(fn_alloc a, size_t context_space) {
		// Allocate the first page.
		//
		size_t length = (std::min(minimum_gc_allocation, sizeof(vm) + context_space) + 0xFFF) >> 12;
		auto*  ptr    = a(nullptr, nullptr, length, false);
		if (!ptr)
			return nullptr;

		// Initialize the GC page, create the VM.
		//
		auto* gc    = new (ptr) gc_page(length);
		vm*   state = gc->create<vm>(context_space);
		util::link_after(&state->gc_page_head, gc);
		return state;
	}

};

// weak ref type?

using namespace lightning;


int main() {
	util::platform_setup_ansi_escapes();

	auto* vm = core::vm::create();
	printf("VM allocated @ %p\n", vm);

	for (auto it = vm->gc_page_head.next; it != &vm->gc_page_head; it = it->next) {
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