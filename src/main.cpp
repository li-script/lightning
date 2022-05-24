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

	static void dump_tokens( std::string_view s ) {
		lexer::state lexer{s};
		size_t       last_line = 0;
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
		puts("");
	}
};

// static constexpr size_t max_argument_count = 16;
// struct function : gc_node<function> {
//	// Function details.
//	//
//	uint32_t         const_counter = 0;  // Number of constants in the upvalue array.
//	uint32_t         num_arguments = 0;  // Vararg if zero, else n-1 args.
//	uint32_t         num_locals    = 0;  // Number of local variables we need to reserve on stack.
//
//	std::vector<any> upvalues;           // Storage of upvalues and constants.
//	// Line defined, snippet name, debug info, bytecode, bla bla bla bla
//
//	// Variable observers.
//	//
//	uint32_t num_constants() const { return const_counter; }
//	uint32_t num_upvalues() const { return upvalues.size() - const_counter; }
//	bool     is_closure() const { return num_upvalues() != 0; }
//
//	// Adds a new constant / upvalue, returns the index.
//	//
//	int32_t add_const(any v) {
//		for (uint32_t i = 0; i != const_counter; i++) {
//			if (upvalues[i] == v) {
//				return -(int32_t) (const_counter - i);
//			}
//		}
//		upvalues.insert(upvalues.begin(), v);
//		return -(int32_t) ++const_counter;
//	}
//	int32_t add_upvalue(any v) {
//		upvalues.push_back(v);
//		return int32_t(num_upvalues()) - 1;
//	}
//
//	// GC enumerator.
//	//
//	template<typename F>
//	void enum_for_gc(F&& fn) {
//		for (auto& v : upvalues) {
//			if (v.is_gc())
//				fn(v.as_gc());
//		}
//	}
// };


#include <optional>


namespace lightning::bc {
// Bytecode definitions.
//
#define LIGHTNING_ENUM_BC(_)                                                           \
	/* Unary operators */                                                               \
	_(LNOT, reg, reg, ___) /* A=!B */                                                   \
	_(AMIN, reg, reg, ___) /* A=-B */                                                   \
	_(MOVE, reg, reg, ___) /* A=B */                                                    \
                                                                                       \
	/* Binary operators.  */                                                            \
	_(AADD, reg, reg, reg) /* A=B+C */                                                  \
	_(ASUB, reg, reg, reg) /* A=B-C */                                                  \
	_(AMUL, reg, reg, reg) /* A=B*C */                                                  \
	_(ADIV, reg, reg, reg) /* A=B/C */                                                  \
	_(AMOD, reg, reg, reg) /* A=B%C */                                                  \
	_(LAND, reg, reg, reg) /* A=B&&C */                                                 \
	_(LOR, reg, reg, reg)  /* A=B||C */                                                 \
	_(SCAT, reg, reg, reg) /* A=B..C */                                                 \
	_(CEQ, reg, reg, reg)  /* A=B==C */                                                 \
	_(CNE, reg, reg, reg)  /* A=B!=C */                                                 \
	_(CLT, reg, reg, reg)  /* A=B<C */                                                  \
	_(CGT, reg, reg, reg)  /* A=B>C */                                                  \
	_(CLE, reg, reg, reg)  /* A=B<=C */                                                 \
	_(CGE, reg, reg, reg)  /* A=B>=C */                                                 \
	_(CTY, reg, reg, imm)  /* A=TYPE(B)==C */                                           \
                                                                                       \
	/* Upvalue operators. */                                                            \
	_(CGET, reg, cst, ___) /* A=CONST[B] */                                             \
	_(UGET, reg, uvl, ___) /* A=UPVAL[B] */                                             \
	_(USET, uvl, reg, ___) /* UPVAL[A]=B */                                             \
                                                                                       \
	/* Table operators. */                                                              \
	_(TNEW, reg, imm, ___) /* A=TABLE{Reserved=B} */                                    \
	_(TDUP, reg, cst, ___) /* A=Duplicate(CONST[B]) */                                  \
	_(TGET, reg, reg, reg) /* A=B[C] */                                                 \
	_(TSET, reg, reg, reg) /* B[C]=A */                                                 \
	_(GGET, reg, reg, ___) /* A=G[B] */                                                 \
	_(GSET, reg, reg, ___) /* G[A]=B */                                                 \
                                                                                       \
	/* Closure operators. */                                                            \
	_(FDUP, reg, cst, reg) /* A=Duplicate(CONST[B]), A.UPVAL[0]=C, A.UPVAL[1]=C+1... */ \
                                                                                       \
	/* Control flow. */                                                                 \
	_(CALL, reg, imm, rel) /* CALL A(B x args @(a+1, a+2...)), JMP C if throw */        \
	_(RETN, reg, imm, ___) /* RETURN A, (IsException:B) */                              \
	_(JMP,  rel, ___, ___) /* JMP A */                                                  \
	_(JCC,  rel, reg, ___) /* JMP A if B */

	// Enum of bytecode ops.
	//
	enum opcode : uint8_t {
#define BC_WRITE(name, a, b, c) name,
		LIGHTNING_ENUM_BC(BC_WRITE)
#undef BC_WRITE
	};

	// Write all descriptors.
	//
	enum class op_t : uint8_t { none, reg, uvl, cst, imm, rel, ___ = none };
	using  op = uint16_t;
	struct desc {
		const char* name;
		op_t        a, b, c;
	};
	static constexpr desc opcode_descs[] = {
#define BC_WRITE(name, a, b, c) {LI_STRINGIFY(name), op_t::a, op_t::b, op_t::c},
		 LIGHTNING_ENUM_BC(BC_WRITE)
#undef BC_WRITE
	};

	// Define the instruction type.
	//
	using imm = int16_t;
	using reg = uint16_t;
	using rel = int16_t;
	struct insn {
		opcode o;
		reg a;
		reg b;
		reg c;

		void print(uint32_t ip) const {
			auto& d = opcode_descs[(uint8_t) o];

			std::optional<rel> rel_pr;
			std::optional<reg> cst_pr;

			auto print_op = [&](op_t o, reg value) LI_INLINE {
				char op[16];
				const char* col = "";
				switch (o) {
					case op_t::none:
						op[0] = 0;
						break;
					case op_t::reg:
						col = LI_RED;
						sprintf_s(op, "r%u", (uint32_t) value);
						break;
					case op_t::rel:
						if (auto r = (rel) value; r > 0) {
							col = LI_GRN; 
							sprintf_s(op, "@%x", ip + r);
						} else {
							col = LI_YLW; 
							sprintf_s(op, "@%x", ip - r);
						}
						rel_pr = (rel) value;
						break;
					case op_t::uvl:
						col = LI_CYN;
						sprintf_s(op, "u%u", (uint32_t) value);
						break;
					case op_t::cst:
						cst_pr = value;
						col    = LI_BLU; 
						sprintf_s(op, "c%u", (uint32_t) value);
						break;
					case op_t::imm:
						col = LI_BLU;
						sprintf_s(op, "$%d", (int32_t) value);
						break;
				}
				printf("%s%-12s " LI_DEF, col, op);
			};

			// IP: OP
			printf(LI_PRP "%05x:" LI_BRG " %-6s", ip, d.name);
			// ... Operands.
			print_op(d.a, a);
			print_op(d.b, b);
			print_op(d.c, c);
			printf("|");

			// TODO: print constant.

			// Special effect for relative.
			//
			if (rel_pr) {
				if (*rel_pr < 0) {
					printf(LI_GRN " v" LI_DEF);
				} else {
					printf(LI_RED " ^" LI_DEF);
				}
			}
			printf("\n");
		}
	};
};


namespace lightning::parser {


	struct local_state {
		core::string* id       = nullptr;
		bool          is_const = false;
	};
	struct func_scope {
		func_scope*              prev   = nullptr;
		std::vector<local_state> locals = {};
	};
	struct func_state : lexer::state {
		func_scope*                scope;
		std::vector<core::string*> upvalues = {};
	};

	enum class expression {

	};

	static void parse_statement( core::vm* L, core::function* f ) {

	}

	static core::function* parse(core::vm* L, std::string_view source) {


		bc::insn{bc::JMP,  bc::reg(bc::rel(-4))}.print(10);
		bc::insn{bc::AADD, 1, 2, 3}.print(11);

		// Setup the function state.
		//
		//func_state

		// Setup the lexer.
		//
		//lightning::lexer::state lexer{source};

		// Create the function.
		//


		return nullptr;
	}
};

// weak ref type?

using namespace lightning;

#include <unordered_map>

int main() {
	platform::setup_ansi_escapes();

	auto* L = core::vm::create();
	printf("VM allocated @ %p\n", L);

	{
		std::ifstream file("S:\\Projects\\Lightning\\parser-test.li");
		std::string   file_buf{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
		debug::dump_tokens(file_buf);

		puts("---------------------------------------\n");
		lightning::parser::parse(L, file_buf);
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

	std::ifstream           file("S:\\Projects\\Lightning\\lexer-test.li");
	std::string             file_buf{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	debug::dump_tokens(file_buf);
#endif
}