#include <lang/lexer.hpp>
#include <lang/operator.hpp>
#include <lang/parser.hpp>
#include <vector>
#include <vm/function.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>

namespace li {
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

	// Label magic flag.
	//
	static constexpr uint32_t label_flag = 0x40000000;

	// Function parser state.
	//
	struct func_scope;
	struct func_state {
		vm*                      L;                     // VM state.
		lex::state&              lex;                   // Lexer state.
		func_scope*              enclosing  = nullptr;  // Enclosing function's scope.
		std::vector<local_state> uvalues    = {};       // Upvalues mapped by name.
		func_scope*              scope      = nullptr;  // Current scope.
		std::vector<any>         kvalues    = {};       // Constant pool.
		bc::reg                  max_reg_id = 1;        // Maximum register ID used.
		std::vector<string*>     args       = {};       // Arguments.
		bool                     is_vararg  = false;    //
		std::vector<bc::insn>    pc         = {};       // Bytecode generated.
		bool                     is_repl    = false;    // Disables locals.
		string*                  decl_name  = nullptr;  // Name.

		// Labels.
		//
		uint32_t                                 next_label = label_flag;  // Next label id.
		std::vector<std::pair<bc::rel, bc::pos>> label_map  = {};          // Maps label id to position.

		func_state(vm* L, lex::state& lex) : L(L), lex(lex) {}
	};

	// Local scope state.
	//
	struct func_scope {
		func_state&              fn;                 // Function that scope belongs to.
		func_scope*              prev;               // Outer scope.
		bc::reg                  reg_next     = 0;   // Next free register.
		std::vector<local_state> locals       = {};  // Locals declared in this scope.
		bc::rel                  lbl_continue = 0;   // Labels.
		bc::rel                  lbl_break    = 0;

		// Emits an instruction and returns the position in stream.
		//
		bc::pos emit(bc::opcode o, bc::reg a = 0, bc::reg b = 0, bc::reg c = 0) {
			fn.pc.emplace_back(bc::insn{o, a, b, c});
			return bc::pos(fn.pc.size() - 1);
		}
		bc::pos emitx(bc::opcode o, bc::reg a, uint64_t xmm) {
			fn.pc.emplace_back(bc::insn{o, a}).xmm() = xmm;
			return bc::pos(fn.pc.size() - 1);
		}

		// Reserves a label identifier.
		//
		bc::rel make_label() { return ++fn.next_label; }

		// Sets a label.
		//
		void set_label_here(bc::rel l) { fn.label_map.emplace_back(l, bc::pos(fn.pc.size())); }

		// Helper for fixing jump targets.
		//
		void jump_here(bc::pos br) { fn.pc[br].a = uint32_t(fn.pc.size()) - (br + 1); }

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
			return std::nullopt;
		}
		std::optional<std::pair<bc::reg, bool>> lookup_uval(string* name) {
			for (size_t i = 0; i != fn.uvalues.size(); i++) {
				if (fn.uvalues[i].id == name) {
					return std::pair<bc::reg, bool>{(bc::reg) i, fn.uvalues[i].is_const};
				}
			}
			return std::nullopt;
		}

		// Inserts a new local variable.
		//
		bc::reg add_local(string* name, bool is_const) {
			auto r = alloc_reg();
			locals.push_back({name, is_const, r});
			return r;
		}

		// Inserts a new constant into the pool.
		//
		std::pair<bc::reg, any> add_const(any c) {
			for (size_t i = 0; i != fn.kvalues.size(); i++) {
				if (fn.kvalues[i] == c) {
					return {(bc::reg) i, fn.kvalues[i]};
				}
			}
			fn.kvalues.emplace_back(c);
			return {(bc::reg) fn.kvalues.size() - 1, c};
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
					emitx(bc::KIMM, r, add_const(v).second.value);
					break;
				}
			}
		}

		// Allocates/frees registers.
		//
		bc::reg alloc_reg(uint32_t n = 1) {
			bc::reg r = reg_next;
			reg_next += n;
			fn.max_reg_id = std::max(fn.max_reg_id, bc::reg(r + n - 1));
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
			if (prev) {
				reg_next     = prev->reg_next;
				lbl_break    = prev->lbl_break;
				lbl_continue = prev->lbl_continue;
			}
		}
		func_scope(const func_scope&)            = delete;
		func_scope& operator=(const func_scope&) = delete;
		~func_scope() { fn.scope = prev; }
	};

	// Writes a function state as a function.
	//
	static function* write_func(func_state& fn, uint32_t line, std::optional<bc::reg> implicit_ret = std::nullopt) {
		// Apply all fixups before writing it.
		//
		for (uint32_t ip = 0; ip != fn.pc.size(); ip++) {
			auto& insn = fn.pc[ip];

			// Skip if flag is not set or it's not actually relative.
			//
			if (!(insn.a & label_flag))
				continue;
			if (bc::opcode_descs[(uint8_t) insn.o].a != bc::op_t::rel)
				continue;

			// Try to fix the label.
			//
			bool fixed = false;
			for (auto& [k, v] : fn.label_map) {
				if (insn.a == k) {
					insn.a = v - (ip + 1);
					fixed  = true;
					break;
				}
			}
			LI_ASSERT_MSG("unresolved label leftover", fixed);
		}

		// If routine does not end with a return, add the implicit return.
		//
		if (fn.pc.empty() || fn.pc.back().o != bc::RET) {
			if (!implicit_ret) {
				fn.pc.push_back({bc::KIMM, 0, -1, -1});
				implicit_ret = 0;
			}
			fn.pc.push_back({bc::RET, *implicit_ret});
		}

		// Create the function value.
		//
		function* f      = function::create(fn.L, fn.pc, fn.kvalues, fn.uvalues.size());
		f->num_locals    = fn.max_reg_id + 1;
		f->num_arguments = (uint32_t) fn.args.size();
		if (fn.decl_name) {
			f->src_chunk = string::format(fn.L, "%.*s:%s", (uint32_t) fn.lex.source_name.size(), fn.lex.source_name.data(), fn.decl_name->c_str());
		} else {
			f->src_chunk = string::create(fn.L, fn.lex.source_name);
		}
		f->src_line      = line;
		return f;
	}

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
		uvl,  // upvalue
		glb,  // global
		idx,  // index into local with another local
	};
	struct upvalue_t {};
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
		expression(std::pair<bc::reg, bool> l) : kind(expr::reg), freeze(l.second), reg(l.first) {}
		expression(upvalue_t, bc::reg u) : kind(expr::uvl), reg(u) {}
		expression(upvalue_t, std::pair<bc::reg, bool> u) : kind(expr::uvl), freeze(u.second), reg(u.first) {}

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
					if (r != reg)
						scope.emit(bc::MOV, r, reg);
					return;
				case expr::imm:
					scope.set_reg(r, imm);
					return;
				case expr::uvl:
					scope.emit(bc::UGET, r, reg);
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

		// Emits a push instruction.
		//
		void push(func_scope& scope) const {
			LI_ASSERT(kind != expr::err);
			switch (kind) {
				case expr::reg:
					scope.emit(bc::PUSHR, reg);
					return;
				case expr::imm:
					switch (imm.type()) {
						case type_none:
						case type_false:
						case type_true:
						case type_number: {
							scope.emitx(bc::PUSHI, 0, imm.value);
							break;
						}
						default: {
							scope.emitx(bc::PUSHI, 0, scope.add_const(imm).second.value);
							break;
						}
					}
					return;
				case expr::uvl:
				case expr::idx:
				case expr::glb:
					auto r = to_nextreg(scope);
					scope.emit(bc::PUSHR, r);
					scope.free_reg(r);
					return;
			}
		}

		// Assigns a value to the lvalue expression.
		//
		void assign(func_scope& scope, const expression& value) const {
			LI_ASSERT(is_lvalue());

			switch (kind) {
				case expr::reg:
					value.to_reg(scope, reg);
					break;
				case expr::uvl: {
					auto val = value.to_anyreg(scope);
					scope.emit(bc::USET, reg, val);
					if (value.kind != expr::reg)
						scope.free_reg(val);
					return;
				}
				case expr::glb: {
					auto val = value.to_anyreg(scope);
					auto idx = scope.alloc_reg();
					scope.set_reg(idx, any(glb));
					scope.emit(bc::GSET, idx, val);
					scope.free_reg(idx);
					if (value.kind != expr::reg)
						scope.free_reg(val);
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
				default:
					assume_unreachable();
			}
		}

		// Prints the expression.
		//
		void print() const {
			switch (kind) {
				case expr::err:
					printf(LI_RED "<err>" LI_DEF);
					break;
				case expr::imm:
					imm.print();
					break;
				case expr::reg:
					if (reg < 0) {
						if (reg == stack_self) {
							printf(LI_GRN "self" LI_DEF);
						} else if (reg == stack_fn) {
							printf(LI_GRN "$F" LI_DEF);
						} else {
							printf(LI_YLW "a%u" LI_DEF, (uint32_t) - (reg + 3));
						}
					} else {
						printf(LI_RED "r%u" LI_DEF, (uint32_t) reg);
					}
					break;
				case expr::uvl:
					if (reg == bc::uval_env) {
						printf(LI_GRN "$E" LI_DEF);
					} else if (reg == bc::uval_glb) {
						printf(LI_GRN "$G" LI_DEF);
					} else {
						printf(LI_GRN "u%u" LI_DEF, (uint32_t) reg);
					}
					break;
				case expr::glb:
					printf(LI_PRP "$G[%s]" LI_DEF, glb->c_str());
					break;
				case expr::idx:
					printf(LI_RED "r%u" LI_DEF, (uint32_t) idx.table);
					printf(LI_CYN "[" LI_DEF);
					printf(LI_RED "r%u" LI_DEF, (uint32_t) idx.field);
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

	// Reads a type name.
	//
	static value_type parse_type(func_scope& scope) {
		if (scope.lex().opt(lex::token_number)) {
			return type_number;
		} else if (scope.lex().opt(lex::token_bool)) {
			return type_false;
		} else if (scope.lex().opt(lex::token_array)) {
			return type_array;
		} else if (scope.lex().opt(lex::token_string)) {
			return type_string;
		} else if (scope.lex().opt(lex::token_userdata)) {
			return type_userdata;
		} else if (scope.lex().opt(lex::token_function)) {
			return type_function;
		} else if (scope.lex().opt(lex::token_thread)) {
			return type_thread;
		} else if (scope.lex().opt(lex::token_table)) {
			return type_table;
		} else {
			scope.lex().error("expected type name.");
			return type_none;
		}
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
			if (exp.kind == expr::err) {
				out = {};
				return nullptr;
			}

			lhs = emit_unop(scope, op->opcode, exp);
		}
		// Otherwise, parse a simple expression.
		//
		else {
			lhs = expr_simple(scope);
			if (lhs.kind == expr::err) {
				out = {};
				return nullptr;
			}
		}

		// Handle special is operator.
		//
		if (scope.lex().opt(lex::token_is)) {
			auto t = parse_type(scope);
			if (t == type_none) {
				return nullptr;
			} else if (lhs.kind == expr::imm) {
				out = expression(any(to_canonical_type_name(lhs.imm.type()) == t));
				return nullptr;
			} else {
				auto r = lhs.to_nextreg(scope);
				scope.emit(bc::CTY, r, r, t);
				out = expression(r);
				return nullptr;
			}
		}

		// Loop until we exhaust the nested binary operators.
		//
		auto op = lookup_operator(scope.lex(), true);
		while (op && op->prio_left < prio) {
			scope.lex().next();

			expression rhs    = {};
			auto       nextop = expr_op(scope, rhs, op->prio_right);
			if (rhs.kind == expr::err) {
				out = {};
				return nullptr;
			}

			lhs = emit_binop(scope, lhs, op->opcode, rhs);
			op  = nextop;
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

	// Parses function declaration, returns the function value.
	//
	static expression parse_function(func_scope& scope, string* name = nullptr);

	// Parses a call, returns the result.
	//
	static expression parse_call(func_scope& scope, const expression& func, const expression& self);

	// Parses block-like constructs, returns the result.
	//
	static expression parse_if(func_scope& scope);
	static expression parse_match(func_scope& scope);
	static expression parse_for(func_scope& scope);
	static expression parse_loop(func_scope& scope);
	static expression parse_while(func_scope& scope);
	static expression parse_env(func_scope& scope);

	// Parses a "statement" expression, which considers both statements and expressions valid.
	// Returns the expression representing the value of the statement.
	//
	static expression expr_stmt(func_scope& scope, bool& fin);

	// Parses a block expression made up of N statements, final one is the expression value
	// unless closed with a semi-colon.
	//
	static expression expr_block(func_scope& scope, bc::reg into = -1);

	// Creates a variable expression.
	//
	static expression expr_var(func_scope& scope, string* name) {
		// Handle special names.
		//
		if (name->view() == "self") {
			return expression(std::pair{stack_self, true});
		} else if (name->view() == "$F") {
			return expression(std::pair{stack_fn, true});
		} else if (name->view() == "$E") {
			return expression(upvalue_t{}, std::pair<bc::reg, bool>{bc::uval_env, true});
		} else if (name->view() == "$G") {
			return expression(upvalue_t{}, std::pair<bc::reg, bool>{bc::uval_glb, true});
		}

		// Try using existing local variable.
		//
		if (auto local = scope.lookup_local(name)) {
			return *local;  // local / arg
		}

		// Try finding an argument.
		//
		for (bc::reg n = 0; n != scope.fn.args.size(); n++) {
			if (scope.fn.args[n] == name) {
				return expression(-3 - n);
			}
		}

		// Try self-reference.
		//
		if (scope.fn.decl_name && name == scope.fn.decl_name) {
			return expression(std::pair{-1, true});
		}

		// Try using existing upvalue.
		//
		if (auto uv = scope.lookup_uval(name)) {
			return *uv;  // uvalue
		}

		// Try borrowing a value by creating an upvalue.
		//
		if (scope.fn.enclosing) {
			expression ex = expr_var(*scope.fn.enclosing, name);
			if (ex.kind != expr::glb) {
				bool    is_const = ex.freeze != 0;
				bc::reg next_reg = (bc::reg) scope.fn.uvalues.size();
				scope.fn.uvalues.push_back({name, is_const, next_reg});
				return expression{upvalue_t{}, {next_reg, is_const}};
			}
		}

		return name;  // global
	}

	// Parses an array literal.
	//
	static expression expr_array(func_scope& scope) {
		// TODO: ADUP const part, dont care rn.

		// Create a new array.
		//
		expression result = scope.alloc_reg();
		auto       allocp = scope.emit(bc::ANEW, result.reg);

		// Until list is exhausted push expressions.
		//
		uint32_t nexpr = 0;
		if (!scope.lex().opt(']')) {
			while (true) {
				reg_sweeper _r{scope};

				expression value = expr_parse(scope);
				if (value.kind == expr::err) {
					return {};
				}
				nexpr++;
				scope.emit(bc::AADD, result.reg, result.reg, value.to_anyreg(scope));

				if (scope.lex().opt(']'))
					break;
				else {
					if (scope.lex().check(',') == lex::token_error) {
						return {};
					}
				}
			}
		}
		scope.fn.pc[allocp].c = nexpr;
		return result;
	}

	// Parses a table literal.
	//
	static expression expr_table(func_scope& scope) {
		// TODO: TDUP const part, dont care rn.

		// Create a new table.
		//
		expression result = scope.alloc_reg();
		auto       allocp = scope.emit(bc::TNEW, result.reg);

		// Until list is exhausted set fields.
		//
		uint32_t nexpr = 0;
		if (!scope.lex().opt('}')) {
			while (true) {
				reg_sweeper _r{scope};

				auto field = scope.lex().check(lex::token_name);
				if (field == lex::token_error) {
					return {};
				}

				expression value;
				if (scope.lex().opt(':')) {
					value = expr_parse(scope);
				} else {
					value = expr_var(scope, field.str_val);
				}
				if (value.kind == expr::err) {
					return {};
				}
				value = value.to_anyreg(scope);

				auto tmp = scope.alloc_reg();
				scope.set_reg(tmp, any(field.str_val));
				scope.emit(bc::TSET, tmp, value.reg, result.reg);
				++nexpr;

				if (scope.lex().opt('}'))
					break;
				else {
					if (scope.lex().check(',') == lex::token_error) {
						return {};
					}
				}
			}
		}
		scope.fn.pc[allocp].c = nexpr;
		return result;
	}

	// Solving ambigious syntax with block vs table.
	//
	static bool is_table_init(func_scope& scope) {
		auto& lex = scope.lex();
		if (lex.lookahead() != lex::token_name) {
			return false;
		}

		// Yes we really need double look-ahead.
		//
		auto pi   = lex.input;
		auto pl   = lex.line;
		auto ll   = lex.scan();
		lex.input = pi;
		lex.line  = pl;
		// printf("TBL INIT SAMPLE: '%s'\n", ll.to_string().c_str());
		return (ll.id == ':' || ll.id == ',');
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
		} else if (tk.id == '[') {
			scope.lex().next();
			base = expr_array(scope);
		} else if (tk.id == '{') {
			if (is_table_init(scope)) {
				scope.lex().next();
				base = expr_table(scope);
			} else {
				scope.lex().next();
				base = expr_block(scope);
			}
		} else if (tk.id == lex::token_if) {
			scope.lex().next();
			base = parse_if(scope);
		} else if (tk.id == lex::token_loop) {
			scope.lex().next();
			base = parse_loop(scope);
		} else if (tk.id == lex::token_while) {
			scope.lex().next();
			base = parse_while(scope);
		} else if (tk.id == lex::token_for) {
			scope.lex().next();
			base = parse_for(scope);
		} else if (tk.id == lex::token_env) {
			scope.lex().next();
			base = parse_env(scope);
		} else {
			scope.lex().error("unexpected token %s", tk.to_string().c_str());
			return {};
		}
		if (base.kind == expr::err)
			return {};

		// Parse suffixes.
		//
		while (true) {
			switch (scope.lex().tok.id) {
				// Call.
				case '{': {
					if (!is_table_init(scope))
						return base;
				}
				case lex::token_lstr:
				case '(': {
					base = parse_call(scope, base, none);
					if (base.kind == expr::err)
						return {};
					break;
				}
				case lex::token_ucall: {
					scope.lex().next();
					auto ftk = scope.lex().check(lex::token_name);
					if (ftk == lex::token_error)
						return {};
					expression func = expr_var(scope, ftk.str_val);
					if (func.kind == expr::err)
						return {};
					base = parse_call(scope, func, base);
					if (base.kind == expr::err)
						return {};
					break;
				}
				case lex::token_icall: {
					scope.lex().next();
					auto ftk = scope.lex().check(lex::token_name);
					if (ftk == lex::token_error)
						return {};
					expression field = any(ftk.str_val);
					expression self = base;
					base            = {base.to_anyreg(scope), field.to_anyreg(scope)};
					base            = parse_call(scope, base, self);
					if (base.kind == expr::err)
						return {};
					break;
				}

				// Index.
				case '[': {
					scope.lex().next();
					expression field = expr_parse(scope);
					if (field.kind == expr::err || scope.lex().check(']') == lex::token_error)
						return {};
					base = {base.to_anyreg(scope), field.to_anyreg(scope)};
					break;
				}
				case '.': {
					scope.lex().next();
					auto ftk = scope.lex().check(lex::token_name);
					if (ftk == lex::token_error)
						return {};
					expression field = any(ftk.str_val);
					base             = {base.to_anyreg(scope), field.to_anyreg(scope)};
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
			case lex::token_lor:
			case '|': {
				return parse_function(scope);
			}
			default: {
				return expr_primary(scope);
			}
		}
	}

	// Parses variable declaration.
	//
	static bool parse_decl(func_scope& scope, bool is_export) {
		// Get token traits and skip it.
		//
		auto keyword  = scope.lex().tok;
		bool is_const = keyword == lex::token_fn || keyword == lex::token_const;
		bool is_func  = keyword == lex::token_fn;
		scope.lex().next();

		// Validate exports.
		//
		if (is_export) {
			if (scope.fn.enclosing) {
				scope.lex().error("export is only valid at top-level declarations.");
				return {};
			}
			if (!is_const) {
				scope.lex().error("expected fn or const after export.");
				return {};
			}
		}
		is_export |= scope.fn.is_repl;

		// If function declaration:
		//
		if (is_func) {
			auto name = scope.lex().check(lex::token_name);
			if (name == lex::token_error) {
				return false;
			}
			if (scope.lex().tok != '(') {
				scope.lex().check('(');
				return false;
			}

			auto    fn  = parse_function(scope, name.str_val);
			bc::reg reg = scope.add_local(name.str_val, is_const);
			fn.to_reg(scope, reg);
			if (is_export) expression(name.str_val).assign(scope, reg);
			return true;
		}

		// If de-structruing assignment:
		//
		bool is_arr = scope.lex().opt('[').has_value();
		bool is_tbl = scope.lex().opt('{').has_value();
		if (is_arr || is_tbl) {
			// Collect variable list.
			//
			std::pair<string*, any> mappings[32];
			size_t                  size = 0;
			if (is_tbl) {
				while (true) {
					if (size == std::size(mappings)) {
						scope.lex().error("too many variables");
						return false;
					}

					// Valid field?
					//
					string* key;
					string* field;
					if (auto ktk = scope.lex().check(lex::token_name); ktk == lex::token_error) {
						return false;
					} else {
						field = key = ktk.str_val;
					}

					// Remapped?
					//
					if (scope.lex().opt(':')) {
						if (auto ktk = scope.lex().check(lex::token_name); ktk == lex::token_error) {
							return false;
						} else {
							key = ktk.str_val;
						}
					}

					// Write the entry.
					//
					mappings[size++] = {key, any(field)};

					// End?
					//
					if (scope.lex().opt('}'))
						break;
					else if (scope.lex().check(',') == lex::token_error)
						return false;
				}
			} else if (is_arr) {
				number next_index = 0;
				while (true) {
					if (size == std::size(mappings)) {
						scope.lex().error("too many variables");
						return false;
					}

					// Skipped.
					//
					if (scope.lex().opt(',')) {
						next_index += 1;
						continue;
					}

					// Valid mapping.
					//
					if (auto name = scope.lex().check(lex::token_name); name != lex::token_error) {
						mappings[size++] = {name.str_val, any(next_index)};
						next_index += 1;
					}
					// Error.
					else {
						return false;
					}

					// End?
					//
					if (scope.lex().opt(']'))
						break;
					else if (scope.lex().check(',') == lex::token_error)
						return false;
				}
			}

			// Require assignment.
			//
			if (scope.lex().check('=') == lex::token_error)
				return false;

			// Parse the expression.
			//
			expression ex = expr_parse(scope);
			if (ex.kind == expr::err) {
				return false;
			}
			ex = ex.to_anyreg(scope);


			// If export set globals as well as locals.
			//
			reg_sweeper _g{scope};
			if (is_export) {
				for (size_t i = 0; i != size; i++) {
					auto reg = scope.add_local(mappings[i].first, is_const);
					expression(mappings[i].second).to_reg(scope, reg);
					scope.emit(bc::TGET, reg, reg, ex.reg);
					expression{mappings[i].first}.assign(scope, expression(reg));
				}
			}
			// Otherwise assign all locals.
			//
			else {
				for (size_t i = 0; i != size; i++) {
					auto reg   = scope.add_local(mappings[i].first, is_const);
					expression(mappings[i].second).to_reg(scope, reg);
					scope.emit(bc::TGET, reg, reg, ex.reg);
				}
			}
			return true;
		}

		// Get the variable name.
		//
		auto var = scope.lex().check(lex::token_name);
		if (var == lex::token_error)
			return false;

		// If immediately assigned:
		//
		if (scope.lex().opt('=')) {
			// Parse the expression.
			//
			expression ex = expr_parse(scope);
			if (ex.kind == expr::err) {
				return false;
			}

			// Push a new local and write it.
			//
			auto reg = scope.add_local(var.str_val, is_const);
			ex.to_reg(scope, reg);

			// If export set global as well as locals.
			//
			if (is_export)
				expression(var.str_val).assign(scope, reg);
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

			// If locals are disabled, this is no-op.
			//
			if (scope.fn.is_repl) {
				return true;
			}

			// Push a new local and load none.
			//
			auto reg = scope.add_local(var.str_val, is_const);
			scope.set_reg(reg, none);
		}
		return true;
	}

	// Parses an optionally assign/compound expression.
	//
	static expression parse_assign_or_expr(func_scope& scope) {
		auto expr = expr_parse(scope);
		if (expr.is_lvalue()) {
			// If simple assignment.
			//
			if (scope.lex().opt('=')) {
				// Throw if const.
				//
				if (expr.kind == expr::reg && expr.freeze) {
					scope.lex().error("assigning to constant variable");
					return {};
				}

				// Parse RHS, propagate errors.
				//
				expression value = expr_parse(scope);
				if (value.kind == expr::err)
					return {};

				// Assign the value.
				//
				expr.assign(scope, value);
			} else {
				// Try finding a compount token matching.
				//
				for (auto& binop : binary_operators) {
					if (binop.compound_token && scope.lex().opt(*binop.compound_token)) {
						// Throw if const.
						//
						if (expr.kind == expr::reg && expr.freeze) {
							scope.lex().error("assigning to constant variable");
							return {};
						}

						// Parse RHS, propagate errors.
						//
						expression value = expr_parse(scope);
						if (value.kind == expr::err)
							return value;

						// Emit the operator, assign the result.
						//
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
	static expression expr_stmt(func_scope& scope, bool& fin) {
		switch (scope.lex().tok.id) {
			// Empty statement => None.
			//
			case ';': {
				return expression(none);
			}

			// Variable declaration => None.
			//
			case lex::token_fn:
			case lex::token_let:
			case lex::token_export:
			case lex::token_const: {
				bool is_export = scope.lex().tok.id == lex::token_export;
				if (is_export)
					scope.lex().next();
				if (!parse_decl(scope, is_export)) {
					return {};
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
				fin = true;

				// void return:
				//
				if (auto& tk = scope.lex().tok; tk.id == ';' || tk.id == lex::token_eof || tk.id == '}') {
					scope.emit(bc::RET, expression(any()).to_anyreg(scope));
				}
				// value return:
				//
				else {
					scope.emit(bc::RET, expr_parse(scope).to_anyreg(scope));
				}
				return expression(none);
			}

			// Throw statement => None.
			//
			case lex::token_throw: {
				scope.lex().next();
				fin = true;

				// void throw:
				//
				if (auto& tk = scope.lex().tok; tk.id == ';' || tk.id == lex::token_eof || tk.id == '}') {
					scope.emit(bc::THRW, expression(any(scope.fn.L->empty_string)).to_anyreg(scope));
				}
				// value return:
				//
				else {
					scope.emit(bc::THRW, expr_parse(scope).to_anyreg(scope));
				}
				return expression(none);
			}

			// Continue/Break.
			//
			case lex::token_continue: {
				scope.lex().next();
				fin = true;

				if (!scope.lbl_continue) {
					scope.lex().error("no loop to continue from");
					return {};
				}
				scope.emit(bc::JMP, scope.lbl_continue);
				return expression(none);
			}
			case lex::token_break: {
				scope.lex().next();
				fin = true;

				if (!scope.lbl_break) {
					scope.lex().error("no loop/switch to break from");
					return {};
				}

				// void return:
				//
				if (auto& tk = scope.lex().tok; tk.id == ';' || tk.id == lex::token_eof || tk.id == '}') {
					scope.emit(bc::JMP, scope.lbl_break);
				}
				// value return:
				//
				else {
					// Find the owning scope.
					//
					auto s = &scope;
					while (s->prev && s->prev->lbl_break == s->lbl_break)
						s = s->prev;

					// Discharge expression to last register at the owning block, jump out.
					//
					expr_parse(scope).to_reg(scope, s->reg_next - 1);
					scope.emit(bc::JMP, scope.lbl_break);
				}
				return expression(none);
			}

			// Anything else gets forward to expression parser.
			//
			default: {
				return expr_parse(scope);
			}
		}
	}

	// Parses a block expression made up of N statements, final one is the expression value
	// unless closed with a semi-colon.
	//
	static expression expr_block(func_scope& pscope, bc::reg into) {
		// Fast path for {}.
		//
		if (pscope.lex().opt('}')) {
			return none;
		}

		expression last{none};
		{
			bool       fin = false;
			func_scope scope{pscope.fn};
			do {
				// If finalized but block is not closed, fail.
				//
				if (fin) {
					pscope.lex().error("unreachable statement");
					return {};
				}

				// Parse a statement, forward error.
				//
				last = expr_stmt(scope, fin);
				if (last.kind == expr::err) {
					return {};
				}

				// If closed with semi-colon, clear value.
				//
				if (scope.lex().opt(';')) {
					last = none;
				}
			} while (!scope.lex().opt('}'));

			// If there is a target register, store.
			//
			if (into != -1) {
				last.to_reg(scope, into);
			}
			// Otherwise if value is not immediate, escape the value from sub-scope.
			//
			else if (last.kind != expr::imm) {
				last.to_reg(scope, pscope.reg_next);
				last = pscope.reg_next;
			}
		}

		// Compensate for the stolen register if relevant and return the result.
		//
		if (into == -1 && last.kind != expr::imm)
			pscope.alloc_reg();
		return last;
	}

	// Parses script body.
	//
	static bool parse_body(func_scope scope) {
		expression last{none};
		bool       fin = false;
		while (scope.lex().tok != lex::token_eof) {
			// If finalized but block is not closed, fail.
			//
			if (fin) {
				scope.lex().error("unreachable statement");
				return false;
			}

			// Parse a statement.
			//
			if (last = expr_stmt(scope, fin); last.kind == expr::err) {
				return false;
			}

			// If closed with semi-colon, clear value.
			//
			if (scope.lex().opt(';')) {
				last = none;
			}
		}

		// Yield last value.
		//
		scope.emit(bc::RET, last.to_anyreg(scope));
		return true;
	}

	// Parses a call, returns the result.
	//
	static expression parse_call(func_scope& scope, const expression& func, const expression& self) {
		using parameter = std::pair<expression, bool>;
		parameter callsite[max_arguments] = {};
		uint32_t  size                    = 0;

		// Allocate temporary site for result.
		//
		bc::reg tmp = scope.alloc_reg(2);

		// Collect arguments.
		//
		if (scope.lex().opt('{')) {
			if (auto ex = expr_table(scope); ex.kind == expr::err) {
				return {};
			} else {
				callsite[size++] = {ex, false};
			}
		} else if (auto lit = scope.lex().opt(lex::token_lstr)) {
			callsite[size++] = {expression(any(lit->str_val)), false};
		} else {
			if (scope.lex().check('(') == lex::token_error)
				return {};
			if (!scope.lex().opt(')')) {
				while (true) {
					if (size == std::size(callsite)) {
						scope.lex().error("too many arguments");
						return {};
					}

					// If reference:
					//
					if (scope.lex().opt(lex::token_ref)) {
						if (auto ex = expr_parse(scope); ex.kind == expr::err)
							return {};
						else if (ex.is_lvalue()) {
							callsite[size++] = {ex, true};
						} else {
							scope.lex().error("expected lvalue to reference argument");
							return {};
						}
					} else {
						if (auto ex = expr_parse(scope); ex.kind == expr::err)
							return {};
						else
							callsite[size++] = {ex, false};
					}

					if (scope.lex().opt(')'))
						break;
					else if (scope.lex().check(',') == lex::token_error)
						return {};
				}
			}
		}

		for (bc::reg i = (size - 1); i >= 0; i--) {
			callsite[i].first.push(scope);
		}
		self.push(scope);
		func.push(scope);
		scope.emit(bc::CALL, size);

		// Reload all references.
		//
		for (bc::reg i = 0; i != size; i++) {
			if (callsite[i].second) {
				auto stack_slot = stack_rsvd + i;
				if (callsite[i].first.kind == expr::reg) {
					scope.emit(bc::SLOAD, callsite[i].first.reg, stack_slot);
				} else {
					scope.emit(bc::SLOAD, tmp + 1, stack_slot);
					callsite[i].first.assign(scope, tmp + 1);
				}
			}
		}

		// Load the result, reset the frame, free all temporaries and return.
		//
		scope.emit(bc::SLOAD, tmp, 0);
		scope.emit(bc::SRST);
		scope.reg_next = tmp + 1;
		return expression(tmp);
	}

	// Parses function declaration, returns the function value.
	//
	static expression parse_function(func_scope& scope, string* name) {
		// Consume the '|', '||' or ')'.
		//
		uint32_t line_num = scope.lex().line;
		auto id = scope.lex().next().id;

		// Create the new function.
		//
		func_state new_fn{scope.fn.L, scope.lex()};
		new_fn.decl_name = name;
		new_fn.enclosing = &scope;
		{
			// Collect arguments.
			//
			func_scope ns{new_fn};
			if (id != lex::token_lor) {
				auto endtk = id == '|' ? '|' : ')';
				if (!ns.lex().opt(endtk)) {
					while (true) {
						auto arg_name = ns.lex().check(lex::token_name);
						if (arg_name == lex::token_error)
							return {};
						new_fn.args.emplace_back(arg_name.str_val);

						if (ns.lex().opt(endtk))
							break;
						else if (ns.lex().check(',') == lex::token_error)
							return {};
					}
				}
			}

			// Parse the result expression.
			//
			expression e = expr_parse(ns);
			if (e.kind == expr::err)
				return {};
			e.to_reg(ns, 0);
		}

		// Write the function with implicit return, insert it into constants.
		//
		function* result = write_func(new_fn, line_num, bc::reg(0));

		// If closure is stateless, simply return as a constant.
		//
		if (result->num_uval == 0) {
			return expression(any(result));
		}

		// Allocate space for the uvalue multi-set.
		//
		auto uv = scope.alloc_reg((uint32_t) new_fn.uvalues.size());
		for (size_t n = 0; n != new_fn.uvalues.size(); n++) {
			expr_var(scope, new_fn.uvalues[n].id).to_reg(scope, uv + (bc::reg) n);
		}
		scope.emit(bc::FDUP, uv, scope.add_const(result).first, uv);

		// Free the unnecessary space and return the function by register.
		//
		scope.free_reg(uv + 1, (uint32_t) new_fn.uvalues.size() - 1);
		return expression(uv);
	}

	// Parses block-like constructs, returns the result.
	//
	static expression parse_if(func_scope& scope) {
		// Fetch the conditional expression, dispatch to ToS.
		//
		auto cc = expr_parse(scope);
		if (cc.kind == expr::err) {
			return {};
		}
		cc = cc.to_nextreg(scope);

		// Define block reader.
		//
		auto block_or_exp = [&]() -> bool {
			if (scope.lex().opt('{')) {
				return expr_block(scope, cc.reg).kind != expr::err;
			} else {
				auto exp = expr_parse(scope);
				if (exp.kind == expr::err)
					return false;
				exp.to_reg(scope, cc.reg);
				return true;
			}
		};

		// Emit the placeholder JCC.
		//
		auto jcc_pos = scope.emit(bc::JNS, 0, cc.reg);

		// Schedule the if block.
		//
		if (!block_or_exp()) {
			return {};
		}

		// Emit the escape jump, fix the JCC.
		//
		auto jmp_pos = scope.emit(bc::JMP);
		scope.jump_here(jcc_pos);

		// If there is an else:
		//
		if (scope.lex().opt(lex::token_else)) {
			if (!block_or_exp()) {
				return {};
			}
		}
		// Otherwise simply null the target.
		//
		else {
			scope.set_reg(cc.reg, any());
		}

		// Fix the escape.
		//
		scope.jump_here(jmp_pos);

		// Yield the register.
		//
		return cc;
	}
	static expression parse_loop(func_scope& scope) {
		// Allocate new continue and break labels.
		//
		auto pb = std::exchange(scope.lbl_break, scope.make_label());
		auto pc = std::exchange(scope.lbl_continue, scope.make_label());

		// Point the continue label at the beginning.
		//
		scope.set_label_here(scope.lbl_continue);

		// Reserve next register for break-with-value, initialize to none.
		//
		auto result = expression{any()}.to_nextreg(scope);

		// Parse the block.
		//
		if (scope.lex().check('{') == lex::token_error) {
			return {};
		}
		if (expr_block(scope).kind == expr::err) {
			return {};
		}

		// Jump to continue.
		//
		scope.emit(bc::JMP, scope.lbl_continue);

		// Emit break label.
		//
		scope.set_label_here(scope.lbl_break);

		// Restore the labels.
		//
		scope.lbl_break    = pb;
		scope.lbl_continue = pc;

		// Return the result.
		//
		return result;
	}
	static expression parse_while(func_scope& scope) {
		// Allocate new continue and break labels.
		//
		auto pb = std::exchange(scope.lbl_break, scope.make_label());
		auto pc = std::exchange(scope.lbl_continue, scope.make_label());

		// Point the continue label at the beginning.
		//
		scope.set_label_here(scope.lbl_continue);

		// Parse the condition, jump to break if not met.
		//
		auto cc = expr_parse(scope);
		if (cc.kind == expr::err) {
			return {};
		}
		cc = cc.to_anyreg(scope);

		// Reserve next register for break-with-value, initialize to none.
		//
		auto result = expression{any()}.to_nextreg(scope);

		// Emit the condition.
		//
		scope.emit(bc::JNS, scope.lbl_break, cc.reg);

		// Parse the block.
		//
		if (scope.lex().check('{') == lex::token_error) {
			return {};
		}
		if (expr_block(scope).kind == expr::err) {
			return {};
		}

		// Jump to continue.
		//
		scope.emit(bc::JMP, scope.lbl_continue);

		// Emit break label.
		//
		scope.set_label_here(scope.lbl_break);

		// Restore the labels.
		//
		scope.lbl_break    = pb;
		scope.lbl_continue = pc;

		// Return the result.
		//
		return result;
	}
	static expression parse_for(func_scope& scope) {
		// Parse the iterator names.
		//
		string* k = nullptr;
		if (auto kn = scope.lex().check(lex::token_name); kn == lex::token_error) {
			return {};
		} else {
			k = kn.str_val;
		}
		string* v = nullptr;
		if (scope.lex().opt(',')) {
			if (auto vn = scope.lex().check(lex::token_name); vn == lex::token_error) {
				return {};
			} else {
				v = vn.str_val;
			}
		}

		// Expect in.
		//
		if (scope.lex().check(lex::token_in) == lex::token_error) {
			return {};
		}

		// Parse the iterated value.
		//
		expression i = expr_parse(scope);
		if (i.kind == expr::err) {
			return {};
		}

		// If numeric for:
		//
		if (auto& tk = scope.lex().tok; tk == lex::token_range || tk == lex::token_rangei) {
			bool inclusive = tk == lex::token_rangei;
			scope.lex().next();

			static constexpr number step = 1;

			// Parse another value.
			//
			expression i2{none};
			if (scope.lex().tok != '{') {
				i2 = expr_parse(scope);
				if (i2.kind == expr::err) {
					return {};
				}
			}

			// Allocate 5 consequtive registers:
			// [it], [max], [step], [<cc>] [<result>]
			//
			auto iter_base = scope.alloc_reg(5);
			i.to_reg(scope, iter_base);
			i2.to_reg(scope, iter_base + 1);
			scope.set_reg(iter_base + 2, step);
			scope.set_reg(iter_base + 4, none);

			// Allocate new continue and break labels and the local.
			//
			auto pb = std::exchange(scope.lbl_break, scope.make_label());
			auto pc = std::exchange(scope.lbl_continue, scope.make_label());
			scope.locals.push_back({k, true, iter_base});

			// Skip the AADD on first entry.
			//
			scope.emit(bc::JMP, 1);

			// Point the continue label at the beginning.
			//
			scope.set_label_here(scope.lbl_continue);

			// Parse the condition, jump to break if we reached the end.
			//
			scope.emit(bc::AADD, iter_base, iter_base, iter_base + 2);  // it = it + step
			if (i2.kind != expr::imm || i2.imm != none) {
				scope.emit(inclusive ? bc::CGT : bc::CGE, iter_base + 3, iter_base, iter_base + 1);  // cc = !cmp(it, max)
				scope.emit(bc::JS, scope.lbl_break, iter_base + 3);
			}

			// Parse the block.
			//
			if (scope.lex().check('{') == lex::token_error) {
				return {};
			}
			if (expr_block(scope).kind == expr::err) {
				return {};
			}

			// Jump to continue.
			//
			scope.emit(bc::JMP, scope.lbl_continue);

			// Emit break label.
			//
			scope.set_label_here(scope.lbl_break);

			// Restore the labels, remove the local.
			//
			scope.lbl_break    = pb;
			scope.lbl_continue = pc;
			scope.locals.pop_back();
			return expression(iter_base + 4);
		}
		// If enumerating for:
		//
		else {
			// Make sure its followed by a block.
			//
			if (scope.lex().check('{') == lex::token_error) {
				return {};
			}

			// Move table to next register, will be assigned the result on completion of the inner scope.
			//
			bc::reg tbl_reg = i.to_nextreg(scope);

			// Start a new scope.
			//
			{
				// Allocate new continue and break labels.
				//
				func_scope iscope{scope.fn};
				iscope.lbl_break    = scope.make_label();
				iscope.lbl_continue = scope.make_label();

				// Allocate 4 consequtive registers:
				// [<istate>, k, v, <result>]
				//
				auto iter_base = iscope.alloc_reg(4);

				// Initialize istate and the result.
				//
				expression(any(iopaque{.bits = 0})).to_reg(iscope, iter_base);
				expression(any()).to_reg(iscope, iter_base + 3);

				// Assign the locals.
				//
				iscope.locals.push_back({k, false, iter_base + 1});
				if (v) {
					iscope.locals.push_back({v, false, iter_base + 2});
				}

				// Point the continue label at the beginning.
				//
				iscope.set_label_here(iscope.lbl_continue);

				// Iterate, jump to break if end.
				//
				iscope.emit(bc::ITER, iscope.lbl_break, iter_base, tbl_reg);

				// Parse the block:
				//
				if (expr_block(iscope).kind == expr::err) {
					return {};
				}

				// Jump to continue.
				//
				iscope.emit(bc::JMP, iscope.lbl_continue);

				// Emit break label.
				//
				iscope.set_label_here(iscope.lbl_break);

				// Move the break result to outer scopes last register.
				//
				expression(iter_base + 3).to_reg(iscope, tbl_reg);
			}

			// Return the result.
			//
			return tbl_reg;
		}
	}

	static expression parse_env(func_scope& scope) {
		// Allocate two registers.
		//
		auto space = scope.alloc_reg(2);

		// Swap environment.
		//
		scope.emit(bc::TNEW, space, 0);
		scope.emit(bc::UGET, space + 1, bc::uval_env);
		scope.emit(bc::USET, bc::uval_env, space);

		// TODO: Should still index the current env, fix when you have metatables.
		//

		// Parse a block and discard the result.
		//
		if (scope.lex().check('{') == lex::token_error) {
			return {};
		}
		if (expr_block(scope).kind == expr::err) {
			return {};
		}

		// Restore environment, free the save slot and return the table as the result.
		//
		scope.emit(bc::USET, bc::uval_env, space + 1);
		scope.reg_next = space + 1;  // Discard block result without making free_reg angry.
		return {space};
	}

	// Parses the code and returns it as a function instance with no arguments on success.
	// If code parsing fails, result is instead a string explaining the error.
	//
	any load_script(vm* L, std::string_view source, std::string_view source_name, bool is_repl) {
		lex::state lx{L, source, source_name};
		func_state fn{L, lx};
		fn.is_repl = is_repl;
		if (!parse_body(fn)) {
			return string::create(L, fn.lex.last_error.c_str());
		} else {
			return write_func(fn, 0);
		}
	}
};