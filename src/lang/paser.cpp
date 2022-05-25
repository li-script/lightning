#include <lang/lexer.hpp>
#include <lang/operator.hpp>
#include <lang/parser.hpp>
#include <vector>
#include <vm/function.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>

namespace lightning::core {

	// Operator traits for parsing.
	//
	static const operator_traits* lookup_operator(lex::state& l, bool binary) {
		std::span<const operator_traits> range;
		if (binary)
			range = {binary_operators};
		else
			range = {unary_operators};

		for (auto& desc : range) {
			if (l.tok.id == desc.token) {
				if (desc.opcode == bc::NOP) {
					l.next();  // Consume, no-op.
					return nullptr;
				} else {
					return &desc;
				}
			}
		}
		return nullptr;
	}

	struct func_scope;
	struct func_state {
		//		func_scope*                enclosing  = nullptr;  // Enclosing function's scope.
		//		std::vector<string*> uvalues    = {};       // Upvalues mapped by name.
		vm*                   L;                     // VM state.
		lex::state            lex;                   // Lexer state.
		func_scope*           scope      = nullptr;  // Current scope.
		std::vector<any>      kvalues    = {};       // Constant pool.
		bc::reg               max_reg_id = 1;        // Maximum register ID used.
		std::vector<string*>  args       = {};       // Arguments.
		bool                  is_vararg  = false;    //
		std::vector<bc::insn> pc         = {};

		func_state(core::vm* L, std::string_view source) : L(L), lex(L, source) {}
	};
	struct local_state {
		string* id       = nullptr;  // Local name.
		bool    is_const = false;    // Set if declared as const.
		bc::reg reg      = 0;        // Register mapping to it.
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
			i.print(uint32_t(fn.pc.size() - 1));  // debug
			return bc::pos(fn.pc.size() - 1);
		}

		// Gets the lexer.
		//
		lex::state& lex() { return fn.lex; }

		// Looks up a variable.
		//
		std::optional<bc::reg> lookup_local(string* name) {
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
		// std::optional<bc::reg> lookup_uval(string* name) {
		//	for (size_t i = 0; i != fn.uvalues.size(); i++) {
		//		if (fn.uvalues[i] == name)
		//			return (bc::reg) i;
		//	}
		// }

		// Inserts a new local variable.
		//
		bc::reg add_local(string* name, bool is_const) {
			auto r = alloc_reg();
			locals.push_back({name, is_const, r});
			return r;
		}

		// Inserts a new constant into the pool.
		//
		bc::reg add_const(any c) {
			for (size_t i = 0; i != fn.kvalues.size(); i++) {
				if (fn.kvalues[i] == c)
					return (bc::reg) i;
			}
			fn.kvalues.emplace_back(c);
			return (bc::reg) fn.kvalues.size() - 1;
		}

		// Loads the constant given in the register in the most efficient way.
		//
		void set_reg(bc::reg r, any v) {
			switch (v.type()) {
				case type_none:
				case type_false:
				case type_true:
				case type_number: {
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
			bc::reg r     = reg_next++;
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
		func_scope(const func_scope&)            = delete;
		func_scope& operator=(const func_scope&) = delete;
		~func_scope() { fn.scope = prev; }
	};

	// Expression type.
	//
	enum class expr : uint8_t {
		error,   // <deferred error, written into lexer state>
		lvalue,  // local
		kvalue,  // constant
		gvalue,  // global
		tvalue,  // index into local with another local
	};
	struct expression {
		expr kind = expr::error;
		union {
			struct {
				bc::reg tbl;
				bc::reg idx;
			} t;
			bc::reg l;
			any     k;
			string* g;
		};

		expression() {}
		expression(bc::reg l) : kind(expr::lvalue), l(l) {}
		expression(any k) : kind(expr::kvalue), k(k) {}
		expression(string* g) : kind(expr::gvalue), g(g) {}
		expression(bc::reg tbl, bc::reg idx) : kind(expr::tvalue), t{tbl, idx} {}

		expression(const expression& o) { memcpy(this, &o, sizeof(expression)); }
		expression& operator=(const expression& o) {
			memcpy(this, &o, sizeof(expression));
			return *this;
		}
	};

	static void expr_toreg(func_scope& scope, const expression& exp, bc::reg reg) {
		LI_ASSERT(exp.kind != expr::error);

		switch (exp.kind) {
			case expr::lvalue:
				scope.emit(bc::MOV, reg, exp.l);
				return;
			case expr::kvalue:
				scope.set_reg(reg, exp.k);
				return;
			case expr::gvalue:
				scope.set_reg(reg, any(exp.g));
				scope.emit(bc::GGET, reg, reg);
				return;
			case expr::tvalue:
				scope.emit(bc::TGET, reg, exp.t.idx, exp.t.tbl);
				break;
		}
	}
	static bc::reg expr_tonextreg(func_scope& scope, const expression& exp) {
		auto r = scope.alloc_reg();
		expr_toreg(scope, exp, r);
		return r;
	}
	static bc::reg expr_load(func_scope& scope, const expression& exp) {
		if (exp.kind == expr::lvalue) {
			return exp.l;
		} else {
			return expr_tonextreg(scope, exp);
		}
	}
	static void expr_store(func_scope& scope, const expression& exp, const expression& value) {
		LI_ASSERT(exp.kind != expr::error);

		switch (exp.kind) {
			case expr::lvalue:
				scope.emit(bc::MOV, exp.l, expr_load(scope, value));
				return;
			case expr::gvalue: {
				auto val = expr_load(scope, value);
				auto idx = scope.alloc_reg();
				scope.set_reg(idx, any(exp.g));
				scope.emit(bc::GSET, idx, val);
				scope.reg_next--;  // immediate free.
				return;
			}
			case expr::tvalue:
				scope.emit(bc::TSET, exp.t.idx, expr_load(scope, value), exp.t.tbl);
				break;
			default:
				util::abort("invalid lvalue type");
		}
	}

	// TODO: Export keyword
	static expression parse_expression(func_scope& scope);

	static expression expr_primary(func_scope& scope) {
		expression base = {};
		if (auto& tk = scope.lex().tok; tk.id == lex::token_name) {
			auto var = scope.lex().next().str_val;
			if (auto local = scope.lookup_local(var)) {
				base = *local;
			} else {
				base = var;
			}
		} else {
			printf("Unexpected lexer token for expr_primary:");
			scope.lex().tok.print();
			breakpoint();
			return {};
		}

		// Parse suffixes.
		//
		while (true) {
			switch (scope.lex().tok.id) {
				// TODO: Call is okay too, if indexed afterwards.
				//
				case '[': {
					scope.lex().next();
					expression field = parse_expression(scope);
					scope.lex().check(']');
					base = {expr_load(scope, base), expr_load(scope, field)};
					break;
				}
				case '.': {
					scope.lex().next();
					expression field{scope.lex().check(lex::token_name).str_val};
					base = {expr_load(scope, base), expr_load(scope, field)};
					break;
				}
				default: {
					return base;
				}
			}
		}
		return base;
	}
	static expression expr_simple(func_scope& scope) {
		switch (scope.lex().tok.id) {
			// Literals.
			//
			case lex::token_lnum: {
				return expression(any{scope.lex().next().num_val});
			}
			case lex::token_lstr: {
				return expression(any{scope.lex().next().str_val});
			}
			case lex::token_true: {
				return expression(const_true);
			}
			case lex::token_false: {
				return expression(const_false);
			}
			default: {
				return expr_primary(scope);
			}
		}
	}

	static const operator_traits* expr_binop(func_scope& scope, expression& out, uint8_t prio);

	/*
		error,  // <deferred error, written into lexer state>
		lvalue, // local
		kvalue, // constant
		gvalue, // global
		tvalue, // index into local with another local
	*/

	static expression emit_unop(func_scope& scope, bc::opcode op, const expression& rhs) {
		// Basic constant folding.
		if (rhs.kind == expr::kvalue) {
			auto [v, ok] = apply_unary(scope.fn.L, rhs.k, op);
			if (ok) {
				return expression(v);
			}
		}

		// Load into a register, emit in-place unary, return the reg.
		//
		auto r = expr_tonextreg(scope, rhs);
		scope.emit(op, r, r);
		return expression(r);
	}
	static expression emit_binop(func_scope& scope, const expression& lhs, bc::opcode op, const expression& rhs) {
		// Basic constant folding.
		if (lhs.kind == expr::kvalue && rhs.kind == expr::kvalue) {
			auto [v, ok] = apply_binary(scope.fn.L, lhs.k, rhs.k, op);
			if (ok) {
				return expression(v);
			}
		}

		// Load both into registers, write over LHS and return the reg.
		//
		auto rl = expr_tonextreg(scope, lhs);
		auto rr = expr_load(scope, rhs);
		scope.emit(op, rl, rl, rr);
		return expression(rl);
	}

	static expression expr_unop(func_scope& scope) {
		if (auto op = lookup_operator(scope.lex(), false)) {
			scope.lex().next();

			expression exp = {};
			expr_binop(scope, exp, op->prio_right);
			return emit_unop(scope, op->opcode, exp);
		} else {
			return expr_simple(scope);
		}
	}

	static const operator_traits* expr_binop(func_scope& scope, expression& out, uint8_t prio) {
		auto& lhs = out;

		lhs     = expr_unop(scope);
		auto op = lookup_operator(scope.lex(), true);
		while (op && op->prio_left <= prio) {
			scope.lex().next();

			expression rhs    = {};
			auto       nextop = expr_binop(scope, rhs, op->prio_right);
			lhs               = emit_binop(scope, lhs, op->opcode, rhs);
			op                = nextop;
		}
		return op;
	}

	static expression parse_expression(func_scope& scope) {
		expression r = {};
		expr_binop(scope, r, UINT8_MAX);
		return r;
	}

	static bool parse_local(func_scope& scope, bool is_const) {
		// Get the variable name and intern the string.
		//
		auto var = scope.lex().check(lex::token_name);
		if (var == lex::token_error)
			return false;

		// Push a new local.
		//
		auto reg = scope.add_local(var.str_val, is_const);

		// If immediately assigned:
		//
		if (scope.lex().opt('=')) {
			// Parse the expression.
			//
			expression ex = parse_expression(scope);
			if (ex.kind == expr::error) {
				return false;
			}

			// Write the local.
			//
			expr_toreg(scope, ex, reg);
		}
		// Otherwise:
		//
		else {
			// Error if const.
			//
			if (is_const) {
				scope.lex().error("const '%s' declared with no initial value.", var.str_val->c_str());
				return false;
			}

			// Load nil.
			//
			scope.set_reg(reg, none);
		}
		return true;
	}

	static bool parse_assign_or_expr(func_scope& scope) {
		auto lv = expr_primary(scope);
		if (lv.kind == expr::error) {
			printf("failed parsing expr lvalue for assignment.\n");
			breakpoint();
		}

		if (!scope.lex().opt('=')) {
			printf("Unexpected lexer token for parse_assign_or_expr:");
			scope.lex().tok.print();
			breakpoint();
		}

		expr_store(scope, lv, parse_expression(scope));
		return true;
	}

	static bool parse_statement(func_scope& scope) {
		switch (scope.lex().tok.id) {
			case ';': {
				return true;
			}
			// TODO label, goto
			// TODO break, continue,
			case lex::token_fn: {
				util::abort("fn token NYI.");
			}
			case lex::token_let:
			case lex::token_const: {
				return parse_local(scope, scope.lex().next().id == lex::token_const);
			}
			case lex::token_name: {
				return parse_assign_or_expr(scope);
			}

			default: {
				printf("Unexpected lexer token for statement:");
				scope.lex().tok.print();
				breakpoint();
			}
		}

		// real statements:
		// => fallback to expression with discarded result.
	}

	static bool parse_body(func_scope scope) {
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

	// Parses the code and returns it as a function instance with no arguments on success.
	// If code parsing fails, result is instead a string explaining the error.
	//
	any load_script(core::vm* L, std::string_view source, std::string_view source_name) {
		func_state fn{L, source};
		if (!parse_body(fn)) {
			return string::create(L, fn.lex.last_error.c_str());
		}

		if (fn.pc.empty() || fn.pc.back().o != bc::RETN) {
			fn.pc.push_back({bc::KIMM, 0, -1, -1});
			fn.pc.push_back({bc::RETN, 0});
		}

		function* f   = function::create(L, fn.pc, fn.kvalues, 0);
		f->num_locals = fn.max_reg_id + 1;
		f->src_chunk  = string::create(L, source_name);
		return f;
	}
};