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




#include <optional>
#include <algorithm>
#include <tuple>


namespace lightning::bc {
// Bytecode definitions.
//
#define LIGHTNING_ENUM_BC(_)                                                           \
	/* Unary operators */                                                               \
	_(LNOT, reg, reg, ___) /* A=!B */                                                   \
	_(ANEG, reg, reg, ___) /* A=-B */                                                   \
	_(MOVE, reg, reg, ___) /* A=B */                                                    \
	_(TYPE, reg, reg, ___) /* A=TYPE(B) */                                              \
                                                                                       \
	/* Binary operators.  */                                                            \
	_(AADD, reg, reg, reg) /* A=B+C */                                                  \
	_(ASUB, reg, reg, reg) /* A=B-C */                                                  \
	_(AMUL, reg, reg, reg) /* A=B*C */                                                  \
	_(ADIV, reg, reg, reg) /* A=B/C */                                                  \
	_(AMOD, reg, reg, reg) /* A=B%C */                                                  \
	_(LAND, reg, reg, reg) /* A=B&&C */                                                 \
	_(LOR,  reg, reg, reg) /* A=B||C */                                                 \
	_(SCAT, reg, reg, reg) /* A=B..C */                                                 \
	_(CEQ,  reg, reg, reg) /* A=B==C */                                                 \
	_(CNE,  reg, reg, reg) /* A=B!=C */                                                 \
	_(CLT,  reg, reg, reg) /* A=B<C */                                                  \
	_(CGT,  reg, reg, reg) /* A=B>C */                                                  \
	_(CLE,  reg, reg, reg) /* A=B<=C */                                                 \
	_(CGE,  reg, reg, reg) /* A=B>=C */                                                 \
                                                                                       \
	/* Upvalue operators. */                                                            \
	_(KGET, reg, kvl, ___) /* A=KVAL[B] */                                              \
	_(UGET, reg, uvl, ___) /* A=UVAL[B] */                                              \
	_(USET, uvl, reg, ___) /* UVAL[A]=B */                                              \
                                                                                       \
	/* Table operators. */                                                              \
	_(TNEW, reg, imm, ___) /* A=TABLE{Reserved=B} */                                    \
	_(TDUP, reg, kvl, ___) /* A=Duplicate(KVAL[B]) */                                   \
	_(TGET, reg, reg, reg) /* A=C[B] */                                                 \
	_(TSET, reg, reg, reg) /* C[A]=B */                                                 \
	_(GGET, reg, reg, ___) /* A=G[B] */                                                 \
	_(GSET, reg, reg, ___) /* G[A]=B */                                                 \
                                                                                       \
	/* Closure operators. */                                                            \
	_(FDUP, reg, kvl, reg) /* A=Duplicate(KVAL[B]), A.UVAL[0]=C, A.UVAL[1]=C+1... */    \
                                                                                       \
	/* Control flow. */                                                                 \
	_(CALL, reg, imm, rel) /* CALL A(B x args @(a+1, a+2...)), JMP C if throw */        \
	_(RETN, reg, ___, ___) /* RETURN A */                                               \
	_(THRW, reg, ___, ___) /* THROW A */                                                \
	_(JMP,  rel, ___, ___) /* JMP A */                                                  \
	_(JCC,  rel, reg, ___) /* JMP A if B */                                             \
	_(BP,   ___, ___, ___) /* Breakpoint */

	// Enum of bytecode ops.
	//
	enum opcode : uint8_t {
#define BC_WRITE(name, a, b, c) name,
		LIGHTNING_ENUM_BC(BC_WRITE)
#undef BC_WRITE
	};

	// Write all descriptors.
	//
	enum class op_t : uint8_t { none, reg, uvl, kvl, imm, rel, ___ = none };
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
	using imm = int32_t;
	using reg = int32_t;
	using rel = int32_t;
	struct insn {
		opcode o;
		reg    a = 0;
		reg    b = 0;
		reg    c = 0;

		void print(uint32_t ip) const {
			auto& d = opcode_descs[(uint8_t) o];

			std::optional<rel> rel_pr;
			std::optional<reg> kvl_pr;

			auto print_op = [&](op_t o, reg value) LI_INLINE {
				char op[16];
				const char* col = "";
				switch (o) {
					case op_t::none:
						op[0] = 0;
						break;
					case op_t::reg:
						if (value >= 0) {
							col = LI_RED;
							sprintf_s(op, "r%u", (uint32_t) value);
						} else {
							col = LI_YLW;
							sprintf_s(op, "a%u", (uint32_t) - (value + 1));
						}
						break;
					case op_t::rel:
						if (value >= 0) {
							col = LI_GRN; 
							sprintf_s(op, "@%x", ip + value);
						} else {
							col = LI_YLW; 
							sprintf_s(op, "@%x", ip - value);
						}
						rel_pr = (rel) value;
						break;
					case op_t::uvl:
						col = LI_CYN;
						sprintf_s(op, "u%u", (uint32_t) value);
						break;
					case op_t::kvl:
						kvl_pr = value;
						col    = LI_BLU; 
						sprintf_s(op, "k%u", (uint32_t) value);
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

namespace lightning::core {
	static constexpr size_t max_argument_count = 16;
	struct function : gc_node<function> {
		static function* create(vm* L, std::span<const bc::insn> opcodes, std::span<const any> kval, uint32_t uval);

		// Function details.
		//
		uint32_t num_uval      = 0;        // Number of upvalues.
		uint32_t num_kval      = 0;        // Number of constants.
		uint32_t num_arguments = 0;        // Vararg if zero, else n-1 args.
		uint32_t num_locals    = 0;        // Number of local variables we need to reserve on stack.
		uint32_t length        = 0;        // Bytecode length.
		uint32_t src_line      = 0;        // Line of definition.
		string*  src_chunk     = nullptr;  // Source of definition.
		table*   environment   = nullptr;  // Table environment.

		// Variable length part.
		//
		bc::insn opcode_array[];
		// any upvalue_array[];
		// any constant_array[];

		// Range observers.
		//
		std::span<bc::insn> opcodes() { return {opcode_array, length}; }
		std::span<any>      uvals() { return {(any*) &opcode_array[length], num_uval}; }
		std::span<any>      kvals() { return {num_uval + (any*) &opcode_array[length], num_kval}; }

		// GC enumerator.
		//
		template<typename F>
		void enum_for_gc(F&& fn) {
			fn(src_chunk);
			for (auto& v : uvals()) {
				if (v.is_gc())
					fn(v.as_gc());
			}
			for (auto& v : kvals()) {
				if (v.is_gc())
					fn(v.as_gc());
			}
		}
	};

	function* function::create(vm* L, std::span<const bc::insn> opcodes, std::span<const any> kval, uint32_t uval) {

		uint32_t routine_length = std::max((uint32_t) opcodes.size(), 1u);
		uint32_t kval_n         = (uint32_t) kval.size();

		// Set function details.
		//
		function* result        = L->alloc<function>(sizeof(bc::insn) * routine_length + sizeof(any) * (uval + kval_n));
		result->num_uval        = uval;
		result->num_kval        = kval_n;
		result->length          = routine_length;
		result->src_chunk       = string::create(L);
		result->opcode_array[0] = bc::insn{bc::BP};
		result->environment     = L->globals;

		// Copy bytecode and constants, <none> initialize upvalues.
		//
		std::copy_n(opcodes.data(), opcodes.size(), result->opcode_array);
		std::copy_n(kval.data(), kval.size(), result->kvals().begin());
		memset(result->uvals().data(), 0xFF, result->uvals().size_bytes());
		return result;
	}

	LI_COLD static any arg_error(vm* L, any a, const char* expected) { return string::format(L, "expected '%s', got '%s'", expected, type_names[a.type()]); }

	// Operators.
	//

	// TODO: Coerce + Meta
	LI_INLINE static std::pair<any, bool> apply_uop(vm* L, any a, bc::op op) {
		switch (op) {
			case bc::LNOT: {
				return {any(!a.as_bool()), true};
			}
			case bc::TYPE: {
				auto t = a.type();
				if (a == const_true)
					t = type_false;
				return {any(number(t)), true};
			}
			case bc::ANEG: {
				if (!a.is(type_number)) [[unlikely]]
					return {arg_error(L, a, "number"), false};
				return {any(-a.as_num()), true};
			}
			default:
				assume_unreachable();
		}
	}
	LI_INLINE static std::pair<any, bool> apply_bop(vm* L, any a, any b, bc::op op) {
		switch (op) {
			case bc::AADD:
				if (!a.is(type_number)) [[unlikely]]
					return {arg_error(L, a, "number"), false};
				if (!b.is(type_number)) [[unlikely]]
					return {arg_error(L, b, "number"), false};
				return {any(a.as_num() + b.as_num()), true};
			case bc::ASUB:
				if (!a.is(type_number)) [[unlikely]]
					return {arg_error(L, a, "number"), false};
				if (!b.is(type_number)) [[unlikely]]
					return {arg_error(L, b, "number"), false};
				return {any(a.as_num() - b.as_num()), true};
			case bc::AMUL:
				if (!a.is(type_number)) [[unlikely]]
					return {arg_error(L, a, "number"), false};
				if (!b.is(type_number)) [[unlikely]]
					return {arg_error(L, b, "number"), false};
				return {any(a.as_num() * b.as_num()), true};
			case bc::ADIV:
				if (!a.is(type_number)) [[unlikely]]
					return {arg_error(L, a, "number"), false};
				if (!b.is(type_number)) [[unlikely]]
					return {arg_error(L, b, "number"), false};
				return {any(a.as_num() / b.as_num()), true};
			case bc::AMOD:
				if (!a.is(type_number)) [[unlikely]]
					return {arg_error(L, a, "number"), false};
				if (!b.is(type_number)) [[unlikely]]
					return {arg_error(L, b, "number"), false};
				return {any(fmod(a.as_num(), b.as_num())), true};
			case bc::LAND:
				return {any(bool(a.as_bool() & b.as_bool())), true};
			case bc::LOR:
				return {any(bool(a.as_bool() | b.as_bool())), true};

			case bc::SCAT: {
				if (!a.is(type_string)) [[unlikely]]
					return {arg_error(L, a, "string"), false};
				if (!b.is(type_string)) [[unlikely]]
					return {arg_error(L, b, "string"), false};
				return {any(string::concat(L, a.as_str(), b.as_str())), true};
			}
			case bc::CEQ:
				return {any(a == b), true};
			case bc::CNE:
				return {any(a != b), true};
			case bc::CLT:
				if (!a.is(type_number)) [[unlikely]]
					return {arg_error(L, a, "number"), false};
				if (!b.is(type_number)) [[unlikely]]
					return {arg_error(L, b, "number"), false};
				return {any(a.as_num() < b.as_num()), true};
			case bc::CGT:
				if (!a.is(type_number)) [[unlikely]]
					return {arg_error(L, a, "number"), false};
				if (!b.is(type_number)) [[unlikely]]
					return {arg_error(L, b, "number"), false};
				return {any(a.as_num() > b.as_num()), true};
			case bc::CLE:
				if (!a.is(type_number)) [[unlikely]]
					return {arg_error(L, a, "number"), false};
				if (!b.is(type_number)) [[unlikely]]
					return {arg_error(L, b, "number"), false};
				return {any(a.as_num() <= b.as_num()), true};
			case bc::CGE:
				if (!a.is(type_number)) [[unlikely]]
					return {arg_error(L, a, "number"), false};
				if (!b.is(type_number)) [[unlikely]]
					return {arg_error(L, b, "number"), false};
				return {any(a.as_num() >= b.as_num()), true};
			default:
				assume_unreachable();
		}
	}



	static bool vm_enter( vm* L, uint32_t n_args ) {

		/*
		arg0
		arg1
		...
		argn
		<fn>
		-- frame begin
		<locals>
		*/

		// Reference the function.
		//
		auto fv = L->peek_stack();
		if (!fv.is(type_function)) [[unlikely]] {
			L->pop_stack_n(n_args + 1);
			L->push_stack(arg_error(L, fv, "function"));
			return false;
		}
		auto* f = fv.as_fun();

		// Allocate locals, reference arguments.
		//
		uint32_t args_begin   = L->stack_top - (n_args + 1);
		uint32_t locals_begin = L->alloc_stack(f->num_locals);

		// Return and ref helpers.
		//
		auto ref_reg = [&](bc::reg r) LI_INLINE -> any& {
			if (r >= 0) {
				LI_ASSERT(f->num_locals > r);
				return L->stack[ locals_begin + r ];
			} else {
				r = -(r + 1);
				// TODO: Arg# does not have to match :(
				return L->stack[args_begin + r];
			}
		};
		auto ref_uval = [&](bc::reg r) LI_INLINE -> any& {
			LI_ASSERT(f->num_uval > r);
			return f->uvals()[r];
		};
		auto ref_kval = [&](bc::reg r) LI_INLINE -> const any& {
			LI_ASSERT(f->num_kval > r);
			return f->kvals()[r];
		};

		auto ret = [&](any value, bool is_exception) LI_INLINE {
			L->stack_top = args_begin;
			L->push_stack(value);
			return is_exception;
		};

		for (uint32_t ip = 0;;) {
			auto [op, a, b, c] = f->opcode_array[ip++];

			switch (op) {
				case bc::TYPE:
				case bc::LNOT:
				case bc::ANEG: {
					auto [r, ok] = apply_uop(L, ref_reg(b), op);
					if (!ok) [[unlikely]]
						return ret(r, true);
					ref_reg(a) = r;
					continue;
				}
				case bc::AADD:
				case bc::ASUB:
				case bc::AMUL:
				case bc::ADIV:
				case bc::AMOD:
				case bc::LAND:
				case bc::LOR:
				case bc::SCAT:
				case bc::CEQ:
				case bc::CNE:
				case bc::CLT:
				case bc::CGT:
				case bc::CLE:
				case bc::CGE: {
					auto [r, ok] = apply_bop(L, ref_reg(b), ref_reg(c), op);
					if (!ok) [[unlikely]]
						return ret(r, true);
					ref_reg(a) = r;
					continue;
				}
				case bc::MOVE: {
					ref_reg(a) = ref_reg(b);
					continue;
				}
				case bc::THRW:
				case bc::RETN: {
					return ret(ref_reg(a), op == bc::THRW);
				}
				case bc::JCC:
					if (!ref_reg(b).as_bool())
						continue;
					[[fallthrough]];
				case bc::JMP:
					ip += a - 1;
					continue;
				case bc::KGET: {
					ref_reg(a) = ref_kval(b);
					continue;
				}
				case bc::UGET: {
					ref_reg(a) = ref_uval(b);
					continue;
				}
				case bc::USET: {
					ref_uval(a) = ref_reg(b);
					continue;
				}
				case bc::TGET: {
					auto tbl = ref_reg(c);
					if (!tbl.is(type_table)) [[unlikely]] {
						return ret(arg_error(L, tbl, "table"), true);
					}
					ref_reg(a) = tbl.as_tbl()->get(L, ref_reg(b));
					continue;
				}
				case bc::TSET: {
					auto tbl = ref_reg(c);
					if (!tbl.is(type_table)) [[unlikely]] {
						return ret(arg_error(L, tbl, "table"), true);
					}
					tbl.as_tbl()->set(L, ref_reg(a), ref_reg(b));
					continue;
				}
				case bc::GGET: {
					ref_reg(a) = f->environment->get(L, ref_reg(b));
					continue;
				}
				case bc::GSET: {
					f->environment->set(L, ref_reg(a), ref_reg(b));
					continue;
				}
				case bc::TNEW: {
					ref_reg(a) = any{table::create(L, b)};
					continue;
				}
				// _(TDUP, reg, cst, ___) /* A=Duplicate(CONST[B]) */
				case bc::TDUP:
				// _(FDUP, reg, cst, reg) /* A=Duplicate(CONST[B]), A.UPVAL[0]=C, A.UPVAL[1]=C+1... */
				case bc::FDUP:
				// _(CALL, reg, imm, rel) /* CALL A(B x args @(a+1, a+2...)), JMP C if throw */
				case bc::CALL:
					util::abort("bytecode '%s' is NYI.", bc::opcode_descs[uint8_t(op)].name);
				case bc::BP:
					breakpoint();
				default:
					break;
			}
		}
	}
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

	std::vector<core::any> constants = {core::any(core::number(2))};
	std::vector<bc::insn>  ins;

	ins.emplace_back(bc::insn{bc::KGET, 0, 0});        // local 0 = const[0]
	ins.emplace_back(bc::insn{bc::AADD, -1, -1, -2});  // arg 1 = arg 1 + arg 2
	ins.emplace_back(bc::insn{bc::AMUL, -1, -1, 0});   // arg 1 = arg 1 + local 0
	ins.emplace_back(bc::insn{bc::RETN, -1});          // ret arg 1

	for (size_t i = 0; i != ins.size(); i++) {
		ins[i].print(i);
	}

	core::function* f = core::function::create(L, ins, constants, 0);
	f->num_locals     = 1;

	L->push_stack(core::number(1));
//	L->push_stack(core::string::format(L, "lol: %d", 4));
	L->push_stack(core::number(2));
	L->push_stack(f);

	if (bool is_ex = core::vm_enter(L, 2)) {
		printf("Execution finished with exception: ");
		debug::print_object(L->pop_stack());
		puts("");
	} else {
		printf("Execution finished with result: ");
		debug::print_object(L->pop_stack());
		puts("");
	}

	//{
	//	std::ifstream file("S:\\Projects\\Lightning\\parser-test.li");
	//	std::string   file_buf{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	//	debug::dump_tokens(file_buf);
	//
	//	puts("---------------------------------------\n");
	//	lightning::parser::parse(L, file_buf);
	//}

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