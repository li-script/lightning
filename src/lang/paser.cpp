#include <lang/lexer.hpp>
#include <lang/operator.hpp>
#include <lang/parser.hpp>
#include <vector>
#include <vm/function.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>

// TODO: Upvalues.
// TODO: Export keyword
// TODO  label, goto
// TODO  numeric for boost instruction 

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

	// Local value descriptor.
	//
	struct local_state {
		string* id       = nullptr;  // Local name.
		bool    is_const = false;    // Set if declared as const.
		bc::reg reg      = 0;        // Register mapping to it.
	};

	// Function parser state.
	//
	struct func_scope;
	struct func_state {
		//	func_scope*          enclosing  = nullptr;// Enclosing function's scope.
		//	std::vector<string*> uvalues    = {};     // Upvalues mapped by name.
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

	// Local scope state.
	//
	struct func_scope {
		func_state&              fn;             // Function that scope belongs to.
		func_scope*              prev;           // Outer scope.
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
		std::optional<std::pair<bc::reg, bool>> lookup_local(string* name) {
			for (auto it = this; it; it = it->prev) {
				for (auto lit = it->locals.rbegin(); lit != it->locals.rend(); ++lit) {
					if (lit->id == name) {
						return std::pair<bc::reg, bool>{lit->reg, lit->is_const};
					}
				}
			}
			for (size_t n = 0; n != fn.args.size(); n++) {
				if (fn.args[n] == name) {
					return std::pair<bc::reg, bool>{-bc::reg(n + 1), true};
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

		// Allocates/frees registers.
		//
		bc::reg alloc_reg(uint32_t n = 1) {
			bc::reg r = reg_next;
			reg_next += n;
			fn.max_reg_id = std::max(fn.max_reg_id, r);
			return r;
		}
		void free_reg(bc::reg r, uint32_t n = 1) {
			LI_ASSERT((r + n) == reg_next);
			reg_next -= n;
		}

		// Construction and destruction in RAII pattern, no copy.
		//
		func_scope(func_state& fn) : fn(fn), prev(fn.scope) {
			fn.scope = this;
			reg_next = prev ? prev->reg_next : 0;
		}
		func_scope(const func_scope&)            = delete;
		func_scope& operator=(const func_scope&) = delete;
		~func_scope() { fn.scope = prev; }
	};

	// Reg-sweep helper, restores register counter on destruction.
	//
	struct reg_sweeper {
		func_scope& s;
		bc::reg     v;
		reg_sweeper(func_scope& s) : s(s), v(s.reg_next) {}
		~reg_sweeper() { s.reg_next = v; }
	};

	// Expression type.
	//
	enum class expr : uint8_t {
		err,  // <deferred error, written into lexer state>
		imm,  // constant
		reg,  // local
		glb,  // global
		idx,  // index into local with another local
	};
	struct expression {
		expr    kind : 7   = expr::err;
		uint8_t freeze : 1 = false;

		union {
			struct {
				bc::reg table;
				bc::reg field;
			} idx;
			bc::reg reg;
			any     imm;
			string* glb;
		};

		// Default constructor maps to error.
		//
		expression() {}

		// Constructed by value.
		//
		expression(bc::reg l) : kind(expr::reg), reg(l) {}
		expression(std::pair<bc::reg, bool> l) : kind(expr::reg), reg(l.first), freeze(l.second) {}
		expression(any k) : kind(expr::imm), imm(k) {}
		expression(string* g) : kind(expr::glb), glb(g) {}
		expression(bc::reg tbl, bc::reg field) : kind(expr::idx), idx{tbl, field} {}

		// Default bytewise copy.
		//
		expression(const expression& o) { memcpy(this, &o, sizeof(expression)); }
		expression& operator=(const expression& o) {
			memcpy(this, &o, sizeof(expression));
			return *this;
		}

		// Returns true if the expression if of type lvalue and can be assigned to.
		//
		bool is_lvalue() const { return kind >= expr::reg; }

		// Returns true if the expression is a dispatched value.
		//
		bool is_value() const { return kind == expr::imm || kind == expr::reg; }

		// Stores the expression value to the specified register.
		//
		void to_reg(func_scope& scope, bc::reg r) const {
			LI_ASSERT(kind != expr::err);

			switch (kind) {
				case expr::reg:
					scope.emit(bc::MOV, r, reg);
					return;
				case expr::imm:
					scope.set_reg(r, imm);
					return;
				case expr::glb:
					scope.set_reg(r, any(glb));
					scope.emit(bc::GGET, r, r);
					return;
				case expr::idx:
					scope.emit(bc::TGET, r, idx.field, idx.table);
					break;
			}
		}

		// Stores the expression value to the next register.
		//
		bc::reg to_nextreg(func_scope& scope) const {
			auto r = scope.alloc_reg();
			to_reg(scope, r);
			return r;
		}

		// References the value using any register.
		//
		bc::reg to_anyreg(func_scope& scope) const {
			if (kind == expr::reg)
				return reg;
			return to_nextreg(scope);
		}

		// Assigns a value to the lvalue expression.
		//
		void assign(func_scope& scope, const expression& value) const {
			LI_ASSERT(is_lvalue());

			switch (kind) {
				case expr::reg:
					value.to_reg(scope, reg);
					break;
				case expr::glb: {
					auto val = value.to_anyreg(scope);
					auto idx = scope.alloc_reg();
					scope.set_reg(idx, any(glb));
					scope.emit(bc::GSET, idx, val);
					scope.free_reg(idx);
					break;
				}
				case expr::idx: {
					if (value.kind == expr::reg) {
						scope.emit(bc::TSET, idx.field, value.reg, idx.table);
					} else {
						auto tv = scope.alloc_reg();
						value.to_reg(scope, tv);
						scope.emit(bc::TSET, idx.field, tv, idx.table);
						scope.free_reg(tv);
					}
					break;
				}
			}
		}

		// Prints the expression.
		//
		void print() const {
			auto print_reg = [](bc::reg reg) {
				if (reg >= 0) {
					printf(LI_RED "r%u" LI_DEF, (uint32_t) reg);
				} else {
					printf(LI_YLW "a%u" LI_DEF, (uint32_t) - (reg + 1));
				}
			};

			switch (kind) {
				case expr::err:
					printf(LI_RED "<err>" LI_DEF);
					break;
				case expr::imm:
					if (imm.is(type_string))
						printf(LI_BLU "\"%s\"" LI_DEF, imm.as_str()->c_str());
					else if (imm.is(type_true))
						printf(LI_BLU "true" LI_DEF);
					else if (imm.is(type_false))
						printf(LI_BLU "false" LI_DEF);
					else if (imm.is(type_none))
						printf(LI_BLU "None" LI_DEF);
					else if (imm.is(type_number))
						printf(LI_BLU "%lf" LI_DEF, imm.as_num());
					else
						printf(LI_BLU "<gc const %p>" LI_DEF, imm.as_gc());
					break;
				case expr::reg:
					print_reg(reg);
					break;
				case expr::glb:
					printf(LI_PRP "_G[%s]" LI_DEF, glb->c_str());
					break;
				case expr::idx:
					print_reg(idx.table);
					printf(LI_CYN "[" LI_DEF);
					print_reg(idx.field);
					printf(LI_CYN "]" LI_DEF);
					break;
				default:
					break;
			}
		}
	};

	// Applies an operator to the expressions handling constant folding, returns the resulting expression.
	//
	static expression emit_unop(func_scope& scope, bc::opcode op, const expression& rhs) {
		// Basic constant folding.
		//
		if (rhs.kind == expr::imm) {
			auto [v, ok] = apply_unary(scope.fn.L, rhs.imm, op);
			if (ok) {
				return expression(v);
			}
		}

		// Apply the operator, load the result into a register and return.
		//
		auto r = scope.alloc_reg();
		if (rhs.kind == expr::reg) {
			scope.emit(op, r, rhs.reg);
		} else {
			rhs.to_reg(scope, r);
			scope.emit(op, r, r);
		}
		return expression(r);
	}
	static expression emit_binop(func_scope& scope, const expression& lhs, bc::opcode op, const expression& rhs) {
		// Basic constant folding.
		//
		if (lhs.kind == expr::imm && rhs.kind == expr::imm) {
			auto [v, ok] = apply_binary(scope.fn.L, lhs.imm, rhs.imm, op);
			if (ok) {
				return expression(v);
			}
		}

		// Apply the operator, load the result into a register and return.
		//
		auto r = scope.alloc_reg();
		if (lhs.kind == expr::reg && rhs.kind == expr::reg) {
			scope.emit(op, r, lhs.reg, rhs.reg);
		} else if (lhs.kind == expr::reg) {
			rhs.to_reg(scope, r);
			scope.emit(op, r, lhs.reg, r);
		} else {
			lhs.to_reg(scope, r);
			scope.emit(op, r, r, rhs.to_anyreg(scope));
		}
		return expression(r);
	}

	// Handles nested binary and unary operators with priorities, finally calls the expr_simple.
	//
	static expression             expr_simple(func_scope& scope);
	static const operator_traits* expr_op(func_scope& scope, expression& out, uint8_t prio) {
		auto& lhs = out;

		// If token matches a unary operator:
		//
		if (auto op = lookup_operator(scope.lex(), false)) {
			// Parse the next expression, emit unary operator.
			//
			scope.lex().next();
			expression exp = {};
			expr_op(scope, exp, op->prio_right);
			lhs = emit_unop(scope, op->opcode, exp);
		}
		// Otherwise, parse a simple expression.
		//
		else {
			lhs = expr_simple(scope);
		}

		// Loop until we exhaust the nested binary operators.
		//
		auto op = lookup_operator(scope.lex(), true);
		while (op && op->prio_left <= prio) {
			scope.lex().next();

			expression rhs    = {};
			auto       nextop = expr_op(scope, rhs, op->prio_right);
			lhs               = emit_binop(scope, lhs, op->opcode, rhs);
			op                = nextop;
		}
		return op;
	}

	// Parses the next expression.
	//
	static expression expr_parse(func_scope& scope) {
		expression r = {};
		expr_op(scope, r, UINT8_MAX);
		return r;
	}

	// Parses a "statement" expression, which considers both statements and expressions valid.
	// Returns the expression representing the value of the statement.
	//
	static expression expr_stmt(func_scope& scope);

	// Parses a block expression made up of N statements, final one is the expression value
	// unless closed with a semi-colon.
	//
	static expression expr_block(func_scope& scope);

	// Creates a variable expression.
	//
	static expression expr_var(func_scope& scope, string* name) {
		if (auto local = scope.lookup_local(name)) {
			return *local; // local / arg
		} else {
			return name;   // global
		}
	}

	// Parses a table literal.
	//
	static expression expr_table(func_scope& scope) {
		// TODO: TDUP const part, dont care rn.

		// Create a new table.
		//
		expression result = scope.alloc_reg();
		scope.emit(bc::TNEW, result.reg);

		// Until list is exhausted set variables.
		//
		while (true) {
			reg_sweeper _r{scope};

			auto field = scope.lex().check(lex::token_name);
			expression value;
			if (scope.lex().opt(':')) {
				value = expr_parse(scope);
			} else {
				value = expr_var(scope, field.str_val);
			}
			value = value.to_anyreg(scope);

			auto tmp = scope.alloc_reg();
			scope.set_reg(tmp, any(field.str_val));
			scope.emit(bc::TSET, tmp, value.reg, result.reg);

			if (scope.lex().opt('}'))
				break;
			else
				scope.lex().check(',');
		}
		return result;
	}

	// Parses a primary expression.
	//
	static expression expr_primary(func_scope& scope) {
		expression base = {};
		if (auto& tk = scope.lex().tok; tk.id == lex::token_name) {
			base = expr_var(scope, scope.lex().next().str_val);
		} else if (tk.id == '(') {
			scope.lex().next();
			base = expr_parse(scope);
			scope.lex().check(')');
		} else if (tk.id == '{') {
			scope.lex().next();
			if (scope.lex().tok == lex::token_name && scope.lex().lookahead().id == ':') {
				base = expr_table(scope);
			} else {
				base = expr_block(scope);
			}
		} else {
			scope.lex().error("unexpected token %s", tk.to_string().c_str());
			return {};
		}
		if (base.kind == expr::err)
			return base;

		// Parse suffixes.
		//
		while (true) {
			switch (scope.lex().tok.id) {
				// TODO: Call a.b() a->b().
				//
				case '[': {
					scope.lex().next();
					expression field = expr_parse(scope);
					if (field.kind == expr::err)
						return field;
					scope.lex().check(']');
					base = {base.to_anyreg(scope), field.to_anyreg(scope)};
					break;
				}
				case '.': {
					scope.lex().next();
					expression field{any(scope.lex().check(lex::token_name).str_val)};
					base = {base.to_anyreg(scope), field.to_anyreg(scope)};
					break;
				}
				default: {
					return base;
				}
			}
		}
		return base;
	}

	// Parses a simple expression with no operators.
	//
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
				scope.lex().next();
				return expression(const_true);
			}
			case lex::token_false: {
				scope.lex().next();
				return expression(const_false);
			}
			default: {
				return expr_primary(scope);
			}
		}
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
			expression ex = expr_parse(scope);
			if (ex.kind == expr::err) {
				return false;
			}

			// Write the local.
			//
			ex.to_reg(scope, reg);
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

	static expression parse_assign_or_expr(func_scope& scope) {
		auto expr = expr_parse(scope);
		if (expr.is_lvalue()) {

			// Throw if const.
			//
			if (expr.kind == expr::reg && expr.freeze) {
				scope.lex().error("assigning to constant variable");
				return {};
			}

			if (scope.lex().opt('=')) {
				expression value = expr_parse(scope);
				if (value.kind == expr::err)
					return value;
				expr.assign(scope, value);
			} else {
				for (auto& binop : binary_operators) {
					if (binop.compound_token && scope.lex().opt(*binop.compound_token)) {
						expression value = expr_parse(scope);
						if (value.kind == expr::err)
							return value;
						expr.assign(scope, emit_binop(scope, expr, binop.opcode, value));
						break;
					}
				}
			}
		}
		return expr;
	}

	// Parses a "statement" expression, which considers both statements and expressions valid.
	// Returns the expression representing the value of the statement.
	//
	static expression expr_stmt(func_scope& scope) {
		switch (scope.lex().tok.id) {

			// Empty statement => None.
			//
			case ';': {
				return expression(none);
			}

			// Variable declaration => None.
			//
			case lex::token_let:
			case lex::token_const: {
				// Forward errors.
				//
				if (!parse_local(scope, scope.lex().next().id == lex::token_const)) {
					return expression();
				}
				return expression(none);
			}

			// Possible assignment, forward to specializer handler.
			//
			case lex::token_name: {
				return parse_assign_or_expr(scope);
			}

			// Return statement => None.
			//
			case lex::token_return: {
				scope.lex().next();

				// void return:
				//
				if (auto& tk = scope.lex().tok; tk.id == ';' || tk.id == lex::token_eof || tk.id == '}') {
					scope.emit(bc::RETN, expression(any()).to_anyreg(scope));	
				}
				// value return:
				//
				else {
					scope.emit(bc::RETN, expr_parse(scope).to_anyreg(scope));	
				}
				return expression(none);
			}

			// <<TODO>>
			case lex::token_continue:
			case lex::token_break:
			case lex::token_fn:
				util::abort("fn token NYI.");

			// Anything else gets forward to expression parser.
			//
			default:
				return expr_parse(scope);
		}
	}

	// Parses a block expression made up of N statements, final one is the expression value
	// unless closed with a semi-colon.
	//
	static expression expr_block(func_scope& pscope) {
		expression last{none};
		{
			func_scope scope{pscope.fn};
			while (!scope.lex().opt('}')) {
				// Parse a statement, forward error.
				//
				last = expr_stmt(scope);
				if (last.kind == expr::err) {
					return last;
				}

				// If closed with semi-colon, clear value.
				//
				if (scope.lex().opt(';')) {
					last = {none};
				}
			}

			// If value is not immediate, escape the value from sub-scope.
			//
			if (last.kind != expr::imm) {
				last.to_reg(scope, pscope.reg_next);
				last = pscope.reg_next;
			}
		}

		// Compensate for the stolen register and return the result.
		//
		if (last.kind != expr::imm)
			pscope.alloc_reg();
		return last;
	}


	static bool parse_body(func_scope scope) {
		while (scope.lex().tok != lex::token_eof) {
			// Parse a statement.
			//
			if (auto e = expr_stmt(scope); e.kind == expr::err) {
				return false;
			}

			// Optionally consume ';', move on to the next one.
			//
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