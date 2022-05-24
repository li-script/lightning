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
};

namespace lightning::core {
	// Opcodes and instructions.
	//
	enum class bytecode : uint8_t {
		// Unary operators.
		//
		LNOT,  // A=!B
		AMIN,  // A=-B
		MOVE,  // A=B

		// Binary operators.
		//
		AADD,  // A=B+C
		ASUB,  // A=B-C
		AMUL,  // A=B*C
		ADIV,  // A=B/C
		AMOD,  // A=B%C
		LAND,  // A=B&&C
		LOR,   // A=B||C
		SCAT,  // A=B..C
		CEQ,   // A=B==C
		CNE,   // A=B!=C
		CLT,   // A=B<C
		CGT,   // A=B>C
		CLE,   // A=B<=C
		CGE,   // A=B>=C

		// Upvalue operators.
		//
		CGET,  // A=CONST[B]
		UGET,  // A=UPVAL[B]
		USET,  // UPVAL[A]=B

		// Table operators.
		//
		TNEW,  // A=TABLE{Reserved=B}
		TDUP,  // A=Duplicate(CONST[B])
		TGET,  // A=B[C]
		TSET,  // B[C]=A
		GGET,  // A=G[B]
		GSET,  // G[A]=B

		// Closure operators.
		//
		FDUP,  // A=Duplicate(CONST[B]), A.UPVAL[0]=C, A.UPVAL[1]=C+1...

		// Control flow.
		//
		CALL,  // CALL A(B x args), JMP C if throw
		RETN,  // RETURN A, (IsException:B)
		JMP,   // JMP A
		JCC,   // JMP A if B
	};
	struct bcinsn {
		bytecode bc;
		uint16_t a;
		uint16_t b;
		uint16_t c;
	};

	static constexpr size_t max_argument_count = 16;
	struct function : gc_node<function> {
		// Function details.
		//
		std::vector<any> upvalues;           // Storage of upvalues and constants.
		uint32_t         const_counter = 0;  // Number of constants in the upvalue array.
		uint32_t         num_arguments = 0;  // Vararg if zero, else n-1 args.
		uint32_t         num_locals    = 0;  // Number of local variables we need to reserve on stack.

		// Line defined, snippet name, debug info, bytecode, bla bla bla bla

		// Variable observers.
		//
		uint32_t num_constants() const { return const_counter; }
		uint32_t num_upvalues() const { return upvalues.size() - const_counter; }
		bool     is_closure() const { return num_upvalues() != 0; }

		// Adds a new constant / upvalue, returns the index.
		//
		int32_t add_const(any v) {
			for (uint32_t i = 0; i != const_counter; i++) {
				if (upvalues[i] == v) {
					return -(int32_t) (const_counter - i);
				}
			}
			upvalues.insert(upvalues.begin(), v);
			return -(int32_t) ++const_counter;
		}
		int32_t add_upvalue(any v) {
			upvalues.push_back(v);
			return int32_t(num_upvalues()) - 1;
		}

		// GC enumerator.
		//
		template<typename F>
		void enum_for_gc(F&& fn) {
			for (auto& v : upvalues) {
				if (v.is_gc())
					fn(v.as_gc());
			}
		}
	};
};
namespace lightning::parser {

	enum class expression {

	};

	static core::function* parse(core::vm* L, std::string_view source) {
		// Setup the lexer.
		//
		lightning::lexer::state lexer{source};

		return nullptr;
	}
};

// weak ref type?

using namespace lightning;

#include <unordered_map>

int main() {
	platform::setup_ansi_escapes();

	auto* L = core::vm::create();

	// std::ifstream           file("S:\\Projects\\Lightning\\parser-test.li");
	// std::string file_buf{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	//  lightning::parser::parse(L, file_buf);

	printf("VM allocated @ %p\n", L);

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

	std::ifstream           file("S:\\Projects\\Lightning\\lexer-test.li");
	std::string             file_buf{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	lightning::lexer::state lexer{file_buf};
	size_t                  last_line = 0;
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