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
		lex::state lexer{s};
		size_t       last_line = 0;
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




#include <optional>
#include <algorithm>
#include <tuple>

#include <vm/bc.hpp>
#include <vm/function.hpp>

namespace lightning::core {

	LI_COLD static any arg_error(vm* L, any a, const char* expected) { return string::format(L, "expected '%s', got '%s'", expected, type_names[a.type()]); }

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
			case bc::APOW:
				if (!a.is(type_number)) [[unlikely]]
					return {arg_error(L, a, "number"), false};
				if (!b.is(type_number)) [[unlikely]]
					return {arg_error(L, b, "number"), false};
				return {any(pow(a.as_num(), b.as_num())), true};
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
			const auto& insn   = f->opcode_array[ip++];
			auto [op, a, b, c] = insn;

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
				case bc::APOW:
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
				case bc::THRW: {
					if (auto& e = ref_reg(a); e != none)
						return ret(e, true);
					continue;
				}
				case bc::RETN: {
					return ret(ref_reg(a), false);
				}
				case bc::JCC:
					if (!ref_reg(b).as_bool())
						continue;
					[[fallthrough]];
				case bc::JMP:
					ip += a - 1;
					continue;
				case bc::KIMM: {
					ref_reg(a) = any(std::in_place, insn.xmm());
					continue;
				}
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
				case bc::TDUP: {
					auto tbl = ref_kval(b);
					LI_ASSERT(tbl.is(type_table));
					ref_reg(a) = tbl.as_tbl()->duplicate(L);
					continue;
				}
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

	struct func_scope;
	struct func_state {
		//		func_scope*                enclosing  = nullptr;  // Enclosing function's scope.
		//		std::vector<core::string*> uvalues    = {};       // Upvalues mapped by name.
		core::vm*                  L;                     // VM state.
		lex::state                 lex;                   // Lexer state.
		func_scope*                scope      = nullptr;  // Current scope.
		std::vector<core::any>     kvalues    = {};       // Constant pool.
		bc::reg                    max_reg_id = 1;        // Maximum register ID used.
		std::vector<core::string*> args       = {};       // Arguments.
		bool                       is_vararg  = false;    //
		std::vector<bc::insn>      pc         = {};
	};
	struct local_state {
		core::string* id       = nullptr;  // Local name.
		bool          is_const = false;    // Set if declared as const.
		bc::reg       reg      = 0;        // Register mapping to it.
	};
	struct func_scope {
		func_state&              fn;             // Function that scope belongs to.
		func_scope*              prev;           // Outer scope.
		bc::reg                  reg_map  = 0;   // Register mapping to the value of the scope.
		bc::reg                  reg_next = 0;   // Next free register.
		std::vector<local_state> locals   = {};  // Locals declared in this scope.

		// Emits an instruction and returns the position in stream.
		//
		bc::pos emit(bc::opcode o, bc::reg a = 0, bc::reg b = 0, bc::reg c = 0) {
			auto& i = fn.pc.emplace_back(bc::insn{o, a, b, c});
			i.print(uint32_t(fn.pc.size() - 1));  // debug
			return bc::pos(fn.pc.size() - 1);
		}
		bc::pos emitx(bc::opcode o, bc::reg a, uint64_t xmm) {
			auto& i = fn.pc.emplace_back(bc::insn{o, a});
			i.xmm() = xmm;
			i.print(uint32_t(fn.pc.size() - 1)); // debug
			return bc::pos(fn.pc.size() - 1);
		}

		// Gets the lexer.
		//
		lex::state& lex() { return fn.lex; }

		// Looks up a variable.
		//
		std::optional<bc::reg> lookup_local(core::string* name) {
			for (auto it = this; it; it = it->prev) {
				for (auto lit = it->locals.rbegin(); lit != it->locals.rend(); ++lit) {
					if (lit->id == name) {
						return lit->reg;
					}
				}
			}
			for (size_t n = 0; n != fn.args.size(); n++) {
				if (fn.args[n] == name) {
					return -bc::reg(n + 1);
				}
			}

			return std::nullopt;
		}
		// std::optional<bc::reg> lookup_uval(core::string* name) {
		//	for (size_t i = 0; i != fn.uvalues.size(); i++) {
		//		if (fn.uvalues[i] == name)
		//			return (bc::reg) i;
		//	}
		// }

		// Inserts a new local variable.
		//
		bc::reg add_local(core::string* name, bool is_const){
			auto r = alloc_reg();
			locals.push_back({name, is_const, r});
			return r;
		}

		// Inserts a new constant into the pool.
		//
		bc::reg add_const(core::any c) {
			for (size_t i = 0; i != fn.kvalues.size(); i++) {
				if (fn.kvalues[i] == c)
					return (bc::reg) i;
			}
			fn.kvalues.emplace_back(c);
			return (bc::reg) fn.kvalues.size();
		}

		// Loads the constant given in the register in the most efficient way.
		//
		void set_reg( bc::reg r, core::any v ) {
			switch (v.type()) {
				case core::type_none:
				case core::type_false:
				case core::type_true:
				case core::type_number: {
					emitx(bc::KIMM, r, v.value);
					break;
				}
				default: {
					emit(bc::KGET, r, add_const(v));
					break;
				}
			}
		}

		// Allocates a register.
		//
		bc::reg alloc_reg() {
			bc::reg r = reg_next++;
			fn.max_reg_id = std::max(fn.max_reg_id, r);
			return r;
		}

		// Construction and destruction in RAII pattern, no copy.
		//
		func_scope(func_state& fn) : fn(fn), prev(fn.scope) {
			fn.scope = this;
			reg_map  = prev ? prev->alloc_reg() : 0;
			reg_next = reg_map + 1;
		}
		func_scope(const func_scope&) = delete;
		func_scope& operator=(const func_scope&) = delete;
		~func_scope() { fn.scope = prev; }
	};


	enum class expr : uint8_t {
		error,  // <deferred error, written into lexer state>
		lvalue, // local
		kvalue, // constant
		gvalue, // global
		tvalue, // index into local with another local
	};
	struct expression {
		expr kind = expr::error;
		union {
			struct {
				bc::reg tbl;
				bc::reg idx;
			} t;
			bc::reg       l;
			core::any     k;
			core::string* g;
		};
	};

	// TODO: Export keyword

	static expression parse_expression(func_scope& scope) {


		return {};
	}


	static bool parse_local(func_scope& scope, bool is_const) {
		// Get the variable name and intern the string.
		//
		auto var = scope.lex().check(lex::token_name);
		if (var == lex::token_error)
			return false;

		// Push a new local.
		//
		auto reg = scope.add_local(core::string::create(scope.fn.L, var.str_val), is_const);

		// If immediately assigned:
		//
		if (scope.lex().opt('=')) {
			expression ex = parse_expression(scope);
			if (ex.kind == expr::error) {
				return false;
			}

			// ex -> discharge to register [reg]
		}
		// Otherwise:
		//
		else {
			// Error if const.
			//
			if (is_const) {
				scope.lex().error("const '%.*s' declared with no initial value.", var.str_val.size(), var.str_val.data());
				return false;
			}

			// Load nil.
			//
			scope.set_reg(reg, core::none);
		}
		return true;
	}

	static bool parse_statement(func_scope& scope) {
		switch (scope.lex().tok.id) {
			case lex::token_fn: {
				util::abort("fn token NYI.");
			}
			case lex::token_let:
			case lex::token_const: {
				return parse_local(scope, scope.lex().next().id == lex::token_const);
			}
			default: {
				printf("Unexpected lexer token for statement:");
				scope.lex().tok.print();
				breakpoint();
			}
		}

		// real statements:
		//  fn
		//  x =
		//  x +=
		//  let x =
		//  const x =
		// => fallback to expression with discarded result.

	}

	static bool parse_body( func_scope scope ) {
		while (scope.lex().tok != lex::token_eof) {
			// Parse a statement.
			if (!parse_statement(scope)) {
				return false;
			}
			// Optionally consume ';', move on to the next one.
			scope.lex().opt(';');
		}
		return true;
	}

	static core::function* parse(core::vm* L, std::string_view source) {


		func_state fn{.L = L, .lex = source};
		if (!parse_body(fn)) {
			util::abort("Lexer error: %s", fn.lex.last_error.c_str());
		}

		// Setup the function state.
		//
		//func_state

		// Setup the lexer.
		//
		//lightning::lex::state lexer{source};

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

	{
		std::ifstream file("..\\parser-test.li");
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

	std::ifstream           file("..\\lexer-test.li");
	std::string             file_buf{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	debug::dump_tokens(file_buf);
#endif
}