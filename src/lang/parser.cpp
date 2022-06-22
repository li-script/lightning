#include <lang/parser.hpp>
#include <vm/function.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <vm/object.hpp>
#include <util/utf.hpp>
#include <lib/std.hpp>
#include <variant>

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

	// Quick throw helper.
	//
	void func_scope::throw_if(const expression& cc, string* msg, bool inv) {
		if (cc.kind == expr::imm && !cc.imm.as_bool()) {
			return;
		}

		auto tmp = alloc_reg();
		cc.to_reg(*this, tmp);

		auto j = emit(inv ? bc::JS : bc::JNS, 0, tmp);
		set_reg(tmp, msg); // line info?
		emit(bc::SETEX, tmp);
		set_reg(tmp, exception_marker);
		emit(bc::RET, tmp);
		jump_here(j);

		reg_next = tmp;
	}

	// Writes a function state as a function.
	//
	static function* write_func(func_state& fn, msize_t line, std::optional<bc::reg> implicit_ret = std::nullopt) {
		// Validate IP.
		//
		LI_ASSERT(fn.pc.size() <= BC_MAX_IP);

		// Apply all fixups before writing it.
		//
		for (msize_t ip = 0; ip != fn.pc.size(); ip++) {
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
				fn.pc.emplace_back(bc::insn{bc::KIMM, 0}).xmm() = nil.value;
				implicit_ret = 0;
			}
			fn.pc.push_back({bc::RET, *implicit_ret});
		}

		// Create the function prototype.
		//
		if (!fn.line_table.empty())
			fn.line_table.front().line_delta -= line;
		function_proto* f = function_proto::create(fn.L, fn.pc, fn.kvalues, fn.line_table);
		f->num_locals    = fn.max_reg_id + 1;
		f->num_uval       = (msize_t) fn.uvalues.size();
		if (fn.decl_name) {
			f->src_chunk = string::format(fn.L, "'%.*s':%s", (uint32_t) fn.lex.source_name.size(), fn.lex.source_name.data(), fn.decl_name->c_str());
		} else if (line != 0) {
			f->src_chunk = string::format(fn.L, "'%.*s':lambda-%u", (uint32_t) fn.lex.source_name.size(), fn.lex.source_name.data(), line);
		} else {
			f->src_chunk = string::format(fn.L, "'%.*s'", (uint32_t) fn.lex.source_name.size(), fn.lex.source_name.data());
		}
		f->src_line      = line;

		// Create the function value.
		//
		function* res = function::create(fn.L, f);
#if LI_JIT
		if (fn.L->jit_all) {
			lib::jit_on(fn.L, res, fn.L->jit_verbose);
		}
#endif
		return res;
	}

	// Applies an operator to the expressions handling constant folding, returns the resulting expression.
	//
	expression emit_unop(func_scope& scope, bc::opcode op, const expression& rhs) {
		// Basic constant folding.
		//
		if (rhs.kind == expr::imm) {
			auto v = apply_unary(scope.fn.L, rhs.imm, op);
			if (!v.is_exc()) {
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
	expression emit_binop(func_scope& scope, const expression& lhs, bc::opcode op, const expression& rhs) {
		// Basic constant folding.
		//
		if (lhs.kind == expr::imm && rhs.kind == expr::imm) {
			auto v = apply_binary(scope.fn.L, lhs.imm, rhs.imm, op);
			if (!v.is_exc()) {
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

	// Parses a primary expression.
	// - If index is set, disables most valid tokens, used to resolve target for operator ::.
	//
	static expression expr_primary(func_scope& scope, bool index = false);

	// Handles nested binary and unary operators with priorities, finally calls the expr_simple.
	//
	static const operator_traits* expr_op(func_scope& scope, expression& out, uint8_t prio);

	// Reads a type name.
	//
	static std::variant<value_type, expression> parse_type_name(func_scope& scope) {
		if (scope.lex().opt(lex::token_number)) {
			return type_number;
		} else if (scope.lex().opt(lex::token_bool)) {
			return type_bool;
		} else if (scope.lex().opt(lex::token_array)) {
			return type_array;
		} else if (scope.lex().opt(lex::token_string)) {
			return type_string;
		} else if (scope.lex().opt(lex::token_object)) {
			return type_object;
		} else if (scope.lex().opt(lex::token_class)) {
			return type_class;
		} else if (scope.lex().opt(lex::token_function)) {
			return type_function;
		} else if (scope.lex().opt(lex::token_table)) {
			return type_table;
		} else if (scope.lex().opt(lex::token_nil)) {
			return type_nil;
		} else {
			return expr_primary(scope, true);
		}
	}

	// Handles nested binary and unary operators with priorities, finally calls the expr_simple.
	//
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
		// Otherwise, parse a primary expression.
		//
		else {
			lhs = expr_primary(scope);
			if (lhs.kind == expr::err) {
				out = {};
				return nullptr;
			}
		}

		// Handle special operators.
		// - IS:
		if (scope.lex().opt(lex::token_is) && 10 < prio) {
			auto t = parse_type_name(scope);
			if (std::holds_alternative<value_type>(t)) {
				if (lhs.kind == expr::imm) {
					out = expression(any(lhs.imm.type() == std::get<value_type>(t)));
					return nullptr;
				} else {
					auto r = lhs.to_nextreg(scope);
					scope.emit(bc::CTY, r, r, std::get<value_type>(t));
					out = expression(r);
					return nullptr;
				}
			} else {
				auto& ex = std::get<expression>(t);
				if (ex.kind == expr::err) {
					return nullptr;
				}
				else if (ex.kind == expr::imm && !ex.imm.is_vcl()) {
					scope.lex().error("expected type name.");
					return nullptr;
				}

				auto regs = scope.alloc_reg(2);
				ex.to_reg(scope, regs + 1);

				if (ex.kind != expr::imm) {
					scope.emit(bc::CTY, regs, regs + 1, type_class);
					scope.throw_if_not(regs, "expected type name");
				}
				lhs.to_reg(scope, regs);

				scope.emit(bc::CTYX, regs, regs, regs + 1);
				out = expression(regs);
				scope.reg_next = regs + 1;
				return nullptr;
			}
		}
		// - IN:
		if (scope.lex().opt(lex::token_in) && 10 < prio) {
			expression rhs  = expr_primary(scope);
			if (rhs.kind == expr::err) {
				out = {};
				return nullptr;
			}

			auto  vin = any(&lib::detail::builtin_in);
			auto result = scope.alloc_reg(); 
			lhs.push(scope);
			rhs.push(scope);
			expression(vin).push(scope);
			scope.emit(bc::CALL, result, 1);
			out = result;
			return nullptr;
		}
		// - Post INC/DEC:
		if (scope.lex().tok == lex::token_cdec || scope.lex().tok == lex::token_cinc) {
			// Throw if const.
			//
			if (!lhs.is_lvalue() || lhs.freeze) {
				scope.lex().error("assigning to constant variable");
				return nullptr;
			}
			auto op   = scope.lex().next() == lex::token_cdec ? bc::ASUB : bc::AADD;
			auto lhsp = lhs.to_nextreg(scope);
			lhs.assign(scope, emit_binop(scope, lhs, op, expression(any(1.0))));
			lhs = lhsp;
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

	// Parses a class declaration, returns the class value.
	//
	static expression parse_class(func_scope& scope, string* name = nullptr);

	// Parses a format string and returns the result.
	//
	static expression parse_format(func_scope& scope);

	// Parses a call, returns the result.
	//
	static expression parse_call(func_scope& scope, const expression& func, const expression& self);

	// Parses block-like constructs, returns the result.
	//
	static expression parse_if(func_scope& scope);
	static expression parse_match(func_scope& scope);
	static expression parse_for(func_scope& scope);
	static expression parse_try(func_scope& scope);
	static expression parse_loop(func_scope& scope);
	static expression parse_while(func_scope& scope);

	// Parses a "statement" expression, which considers both statements and expressions valid.
	// Returns the expression representing the value of the statement.
	//
	static expression expr_stmt(func_scope& scope, bool& fin);

	// Parses a block expression made up of N statements, final one is the expression value
	// unless closed with a semi-colon.
	//
	static expression expr_block(func_scope& scope, bc::reg into = -1, bool no_term = false);

	// Creates a variable expression.
	//
	static expression expr_var(func_scope& scope, string* name, bool is_ucall = false) {
		// If UCALL and next token is '!', ignore.
		//
		if (is_ucall && scope.lex().tok == '!') {
			return name;
		}

		// Handle special names.
		//
		if (name->view() == "self") {
			return expression((int32_t) FRAME_SELF, true);
		} else if (name->view() == "$F") {
			return expression((int32_t) FRAME_TARGET, true);
		} else if (name->view() == "$E") {
			if (scope.fn.module_table) {
				return expression(any(scope.fn.module_table));
			} else {
				return expression(any(scope.fn.scope_table));
			}
		} else if (name->view() == "$MOD") {
			if (scope.fn.module_name) {
				return expression(any(scope.fn.module_name));
			} else {
				return expression(nil);
			}
		} else if (name->view() == "$VA") {
			if (!scope.fn.is_vararg) {
				scope.lex().error("$VA cannot be used in non-vararg function");
				return {};
			}

			// TODO Slice?
			//

			// Handle functions with $VA as argument.
			//
			if (scope.lex().opt(lex::token_ucall)) {
				// ::len()
				auto n = scope.lex().check(lex::token_name);
				if (n == lex::token_error)
					return {};
				if (n.str_val->view() != "len") {
					scope.lex().error("$VA can only be called with len()");
					return {};
				}
				if (scope.lex().check('(') == lex::token_error || scope.lex().check(')') == lex::token_error) {
					return {};
				}

				auto tmp = scope.alloc_reg(2);
				scope.emit(bc::VACNT, tmp);
				scope.set_reg(tmp + 1, number(scope.fn.args.size()));
				scope.emit(bc::ASUB, tmp, tmp, tmp + 1);
				scope.reg_next = tmp + 1;
				return expression(tmp);
			}
			// Expect index immediately.
			//
			else {
				if (scope.lex().check('[') == lex::token_error) {
					return {};
				}

				// Parse index, make sure it is an integer.
				//
				auto       tmp = scope.alloc_reg(2);
				expression idx = expr_parse(scope);
				idx.to_reg(scope, tmp + 1);

				if (idx.kind != expr::imm || !idx.imm.is_num()) {
					scope.emit(bc::CTY, tmp, tmp + 1, type_number);
					scope.throw_if_not(tmp, "expected numeric index");
				}

				// Increment index by fixed argument count.
				//
				scope.set_reg(tmp, number(scope.fn.args.size()));
				scope.emit(bc::AADD, tmp + 1, tmp, tmp + 1);

				// Skip the ']'.
				//
				if (scope.lex().check(']') == lex::token_error) {
					return {};
				}

				// Read the argument and return.
				//
				scope.emit(bc::VAGET, tmp, tmp + 1);
				scope.reg_next = tmp + 1;
				return expression(tmp);
			}
		}

		// Try using existing local variable.
		//
		for (auto it = &scope; it; it = it->prev) {
			for (auto lit = it->locals.rbegin(); lit != it->locals.rend(); ++lit) {
				if (lit->id == name) {
					if (lit->reg == -1)
						return lit->cxpr;
					else
						return expression(lit->reg, lit->is_const);
				}
			}
		}

		// Try finding an argument.
		//
		for (bc::reg n = 0; n != scope.fn.args.size(); n++) {
			if (scope.fn.args[n].name == name) {
				return expression(int32_t(-FRAME_SIZE - (n + 1)));
			}
		}

		// Try self-reference.
		//
		if (scope.fn.decl_name && name == scope.fn.decl_name) {
			return expression((int32_t) FRAME_TARGET, true);
		}

		// Try using existing upvalue.
		//
		for (bc::reg i = 0; i != scope.fn.uvalues.size(); i++) {
			if (scope.fn.uvalues[i].id == name) {
				return expression(upvalue_t{}, (bc::reg) i, scope.fn.uvalues[i].is_const);  // uvalue
			}
		}

		// Try finding a builtin.
		//
		any bt = scope.fn.L->modules->get(scope.fn.L, (any) string::create(scope.fn.L, "builtin"));
		if (bt.is_tbl()) {
			bt = bt.as_tbl()->get(scope.fn.L, (any) name);
			if (bt != nil) {
				return bt;
			}
		}

		// Try borrowing a value by creating an upvalue.
		//
		if (scope.fn.enclosing) {
			expression ex = expr_var(*scope.fn.enclosing, name);
			if (ex.kind != expr::env) {
				if (ex.kind == expr::imm)
					return ex;
				bool    is_const = ex.freeze != 0;
				bc::reg next_reg = (bc::reg) scope.fn.uvalues.size();
				scope.fn.uvalues.push_back({name, is_const, next_reg});
				return expression{upvalue_t{}, next_reg, is_const};
			}
		}
		return name;  // global
	}

	// Validates the type of an expression.
	//
	static bool type_check_var(func_scope& scope, string* name, const std::variant<value_type, expression>& ty) {
		// Load the argument into a register.
		//
		auto ex = expr_var(scope, name);
		if (ex.kind == expr::err)
			return false;
		auto r  = ex.to_anyreg(scope);
		auto cc = scope.alloc_reg();

		// Validate the type or throw.
		//
		std::string_view ty_str;
		if (std::holds_alternative<value_type>(ty)) {
			auto vt = std::get<value_type>(ty);
			scope.emit(bc::CTY, cc, r, vt);
			ty_str = type_names[vt];
		} else {
			auto& ex = std::get<expression>(ty);
			if (ex.kind == expr::err)
				return false;
			if (ex.kind != expr::imm || !ex.imm.is_vcl()) {
				scope.lex().error("expected constant type name.");
				return false;
			}
			ex.to_reg(scope, cc);
			scope.emit(bc::CTYX, cc, r, cc);
			ty_str = get_type_name(scope.fn.L, type(ex.imm.as_vcl()->vm_tid));
		}
		scope.throw_if_not(cc, "expected variable '%s' to be of type '%.*s'", name->c_str(), ty_str.size(), ty_str.data());
		return true;
	}

	// Creates an index expression.
	//
	static expression expr_index(func_scope& scope, const expression& obj, const expression& key, bool handle_null = false) {
		if (obj.kind == expr::imm && key.kind == expr::imm) {
			if (obj.imm.is_tbl()) {
				if (obj.imm.as_tbl()->is_frozen) {
					return (any)obj.imm.as_tbl()->get(scope.fn.L, key.imm);
				}
			}
		}
		if (!handle_null)
			return {obj.to_anyreg(scope), key.to_anyreg(scope)};

		if (obj.kind == expr::imm && obj.imm == nil) {
			return nil;
		}

		auto res = scope.alloc_reg();
		auto cc = scope.alloc_reg();
		auto o = obj.to_anyreg(scope);
		auto k = key.to_anyreg(scope);

		scope.set_reg(res, nil);
		scope.emit(bc::CEQ, cc, o, res);
		auto j = scope.emit(bc::JS, 0, cc);
		scope.emit(bc::TGET, res, k, o);
		scope.jump_here(j);
		scope.reg_next = res + 1;
		return expression{res, true};
	}

	// Parses an array literal.
	//
	static expression expr_array(func_scope& scope) {
		// TODO: Duplicate template.

		// Create a new array.
		//
		expression result = scope.alloc_reg();
		auto       allocp = scope.emit(bc::ANEW, result.reg);

		// Until list is exhausted push expressions.
		//
		msize_t nexpr = 0;
		if (!scope.lex().opt(']')) {
			while (true) {
				reg_sweeper _r{scope};

				expression value = expr_parse(scope);
				if (value.kind == expr::err) {
					return {};
				}

				expression id{any(number(nexpr++))};
				scope.emit(bc::TSETR, id.to_anyreg(scope), value.to_anyreg(scope), result.reg);

				if (scope.lex().opt(']'))
					break;
				else {
					if (scope.lex().check(',') == lex::token_error) {
						return {};
					}
				}
			}
		}
		scope.fn.pc[allocp].b = nexpr;
		return result;
	}

	// Parses a table literal.
	//
	static expression expr_table(func_scope& scope) {
		// TODO: Duplicate template.

		// Create a new table.
		//
		expression result = scope.alloc_reg();
		auto       allocp = scope.emit(bc::TNEW, result.reg);

		// Until list is exhausted set fields.
		//
		msize_t nexpr = 0;
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
				scope.emit(bc::TSETR, tmp, value.reg, result.reg);
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
		scope.fn.pc[allocp].b = nexpr;
		return result;
	}

	// Solving ambigious syntax with block vs table.
	//
	static bool is_table_init(func_scope& scope) {
		auto& lex = scope.lex();
		if (auto lh = lex.lookahead(); lh == '}') {
			return !scope.first_scope;
		} else if (lh != lex::token_name) {
			return false;
		}

		// Yes we really need double look-ahead.
		//
		auto pi   = lex.input;
		auto pl   = lex.line;
		auto ll   = lex.scan();
		lex.input = pi;
		lex.line  = pl;
		return (ll.id == ':' || ll.id == ',');
	}

	// Parses a primary expression.
	// - If index is set, disables most valid tokens, used to resolve target for operator ::.
	//
	static expression expr_primary(func_scope& scope, bool index) {
		expression base = {};

		// Handle pre inc/dec specially.
		//
		if (scope.lex().tok == lex::token_cdec || scope.lex().tok == lex::token_cinc) {
			auto op = scope.lex().next() == lex::token_cdec ? bc::ASUB : bc::AADD;
			base = expr_primary(scope);
			if (base.kind == expr::err) {
				return {};
			} else if (!base.is_lvalue() || base.freeze) {
				scope.lex().error("assigning to constant variable");
				return {};
			}
			base.assign(scope, emit_binop(scope, base, op, expression(any(1.0))));
		}
		// Variables.
		//
		else if (auto& tk = scope.lex().tok; tk.id == lex::token_name) {
			base = expr_var(scope, scope.lex().next().str_val, index);
		}
		// Sub expressions.
		//
		else if (tk.id == '(') {
			scope.lex().next();
			base = expr_parse(scope);
			scope.lex().check(')');
		}
		// -- Anything below this is not valid for index expression.
		else if (index) {
			scope.lex().error("unexpected token %s", tk.to_string().c_str());
			return {};
		}
		// Constructs.
		//
		else if (tk.id == lex::token_if) {
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
		} else if (tk.id == lex::token_try) {
			scope.lex().next();
			base = parse_try(scope);
		}
		// Literals.
		//
		else if (tk.id == '[') {
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
		}
		else if (tk.id == lex::token_lnum) {
			base = expression(any{scope.lex().next().num_val});
		} else if (tk.id == lex::token_lstr) {
			base = expression(any{scope.lex().next().str_val});
		} else if (tk.id == lex::token_fstr) {
			base = parse_format(scope);
		} else if (tk.id == lex::token_true) {
			scope.lex().next();
			base = expression(const_true);
		} else if (tk.id == lex::token_false) {
			scope.lex().next();
			base = expression(const_false);
		} else if (tk.id == lex::token_nil) {
			scope.lex().next();
			base = expression(nil);
		} else if (tk.id == lex::token_lor || tk.id == '|') {
			base = parse_function(scope);
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
					if (index) {
						return base;
					}

					base = parse_call(scope, base, nil);
					if (base.kind == expr::err)
						return {};
					break;
				}
				case lex::token_ucall: {
					if (index) {
						scope.lex().error("unexpected token %s", scope.lex().tok.to_string().c_str());
						return {};
					}

					scope.lex().next();
					expression func = expr_primary(scope, true);
					if (func.kind == expr::err)
						return {};
					base = parse_call(scope, func, base);
					if (base.kind == expr::err)
						return {};
					break;
				}
				case lex::token_icall: {
					if (index) {
						scope.lex().error("unexpected token %s", scope.lex().tok.to_string().c_str());
						return {};
					}

					scope.lex().next();
					auto ftk = scope.lex().check(lex::token_name);
					if (ftk == lex::token_error)
						return {};
					expression field = any(ftk.str_val);
					expression self = base;
					base             = expr_index(scope, base, field);
					base             = parse_call(scope, base, self);
					if (base.kind == expr::err)
						return {};
					break;
				}

				// Index.
				case lex::token_idxif:
				case '[': {
					bool       nullish = scope.lex().next() == lex::token_idxif;
					expression field   = expr_parse(scope);
					if (field.kind == expr::err || scope.lex().check(']') == lex::token_error)
						return {};
					base = expr_index(scope, base, field, nullish);
					break;
				}
				case lex::token_idxlif:
				case '.': {
					bool nullish = scope.lex().next() == lex::token_idxlif;
					auto ftk = scope.lex().check(lex::token_name);
					if (ftk == lex::token_error)
						return {};
					expression field = any(ftk.str_val);
					base             = expr_index(scope, base, field, nullish);
					break;
				}
				default: {
					return base;
				}
			}
		}
		return base;
	}

	// Parses variable declaration.
	//
	static bool parse_decl(func_scope& scope, bool is_export) {
		// Get token traits and skip it.
		//
		auto keyword  = scope.lex().tok;
		bool is_func  = keyword == lex::token_fn;
		bool is_class = keyword == lex::token_class;
		bool is_const = is_func || is_class || keyword == lex::token_const;
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

		// If function or class declaration:
		//
		if (is_func || is_class) {
			auto name = scope.lex().check(lex::token_name);
			if (name == lex::token_error) {
				return false;
			}

			if (is_func && scope.lex().tok != '(') {
				scope.lex().check('(');
				return false;
			}

			auto res = (is_func ? &parse_function : &parse_class)(scope, name.str_val);
			if (res.kind == expr::err) {
				return {};
			}

			expression result;
			if (is_const && res.kind == expr::imm) {
				scope.add_local_cxpr(name.str_val, res.imm);
				result = res.imm;
			} else {
				bc::reg reg = scope.add_local(name.str_val, is_const);
				res.to_reg(scope, reg);
				result = reg;
			}
			if (is_export)
				expression(export_t{}, name.str_val).assign(scope, result);
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
			if (is_export) {
				for (size_t i = 0; i != size; i++) {
					auto        reg = scope.add_local(mappings[i].first, is_const);
					reg_sweeper _g{scope};
					expression(mappings[i].second).to_reg(scope, reg);
					scope.emit(is_arr ? bc::TGETR : bc::TGET, reg, reg, ex.reg);
					expression{export_t{}, mappings[i].first}.assign(scope, expression(reg));
				}
			}
			// Otherwise assign all locals.
			//
			else {
				for (size_t i = 0; i != size; i++) {
					auto        reg = scope.add_local(mappings[i].first, is_const);
					reg_sweeper _g{scope};
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

		// If there is a type annotation parse it:
		//
		std::optional<std::variant<value_type, expression>> ty_guard;
		if (scope.lex().opt(':')) {
			ty_guard = parse_type_name(scope);
		}

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
			expression result;
			if (ex.kind == expr::imm && is_const && ex.imm != nil) {
				scope.add_local_cxpr(var.str_val, ex.imm);
				result = ex;
			} else {
				auto reg = scope.add_local(var.str_val, is_const);
				ex.to_reg(scope, reg);
				result = reg;
			}

			// Insert the type-check.
			//
			if (ty_guard && !type_check_var(scope, var.str_val, *ty_guard)) {
				return false;
			}

			// If export set global as well.
			//
			if (is_export)
				expression(export_t{}, var.str_val).assign(scope, result);
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

			// Error if type checked.
			//
			if (ty_guard) {
				scope.lex().error("'%s' with type annotation declared with no initial value.", var.str_val->c_str());
				return false;
			}

			// If locals are disabled, this is no-op.
			//
			if (scope.fn.is_repl) {
				return true;
			}

			// Push a new local and load nil.
			//
			auto reg = scope.add_local(var.str_val, is_const);
			scope.set_reg(reg, nil);
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
				return expression(nil);
			}

			// Variable declaration => None.
			//
			case lex::token_class:
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
				return expression(nil);
			}

			// Import => None.
			//
			case lex::token_import: {
				if (scope.fn.enclosing) {
					scope.lex().error("import is only valid at top-level declarations.");
					return {};
				}
				scope.lex().next();

				bool is_optional = scope.lex().opt('?').has_value();
				auto name = scope.lex().opt(lex::token_lstr);
				if (!name) {
					name = scope.lex().check(lex::token_name);
				}
				if (*name == lex::token_error) {
					return {};
				}

				auto mod = scope.fn.L->modules->get(scope.fn.L, (any) name->str_val);
				if (mod == nil) {
					if (scope.fn.L->import_fn) {
						mod = scope.fn.L->import_fn(scope.fn.L, scope.lex().source_name, name->str_val->view());
					}
					if (mod == nil || mod.is_exc()) {
						if (!is_optional) {
							if (mod == nil)
								scope.lex().error("module '%s' not found", name->str_val->c_str());
							else
								scope.lex().error(scope.fn.L->last_ex.coerce_str(scope.fn.L)->c_str());
							return {};
						}
						mod = nil;
					}
				}

				auto alias = *name;
				if (scope.lex().opt(lex::token_as)) {
					alias = scope.lex().check(lex::token_name);
					if (alias == lex::token_error) {
						return {};
					}
				}

				scope.add_local_cxpr(alias.str_val, mod);
				if (scope.fn.is_repl) {
					expression{alias.str_val}.assign(scope, (any) mod);
				}
				return expression(nil);
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
					scope.emit(bc::RET, expression(nil).to_anyreg(scope));
				}
				// value return:
				//
				else {
					scope.emit(bc::RET, expr_parse(scope).to_anyreg(scope));
				}
				return expression(nil);
			}

			// Throw statement => None.
			//
			case lex::token_throw: {
				scope.lex().next();
				fin = true;

				// void throw:
				//
				expression res;
				if (auto& tk = scope.lex().tok; tk.id == ';' || tk.id == lex::token_eof || tk.id == '}') {
					res = expression(nil);
				}
				// value throw:
				//
				else {
					res = expr_parse(scope);
				}

				// If there is a catch pad, just jump to it, otherwise return.
				//
				auto tmp = scope.alloc_reg();
				if (res.kind == expr::reg) {
					scope.emit(bc::SETEX, res.reg);
				} else {
					res.to_reg(scope, tmp);
					scope.emit(bc::SETEX, tmp);
				}

				if (scope.lbl_catchpad != 0) {
					scope.emit(bc::JMP, scope.lbl_catchpad);
				} else {

					scope.set_reg(tmp, exception_marker);
					scope.emit(bc::RET, tmp);
				}
				scope.free_reg(tmp);
				return expression(nil);
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
				return expression(nil);
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
				return expression(nil);
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
	static expression expr_block(func_scope& pscope, bc::reg into, bool no_term) {
		// Fast path for {}.
		//
		if (pscope.lex().tok == '}') {
			if (!no_term)
				pscope.lex().next();
			return nil;
		}

		expression last{nil};
		{
			bool       fin = false;
			func_scope scope{pscope.fn};
			while (true) {
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
					last = nil;
				}
				if (scope.lex().tok == '}') {
					if (!no_term)
						scope.lex().next();
					break;
				}
			};

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
		expression last{nil};
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
				last = nil;
			}
		}

		// Yield last value.
		//
		scope.emit(bc::RET, last.to_anyreg(scope));
		return true;
	}
	
	// Attempts to constant fold a call into a native builtin.
	//
	static expression parse_call_cxpr(vm* L, std::span<expression> callsite, function* func, expression self) {
		// Push all values and call into the handler.
		//
		for (size_t i = callsite.size(); i != 0; i--) {
			L->push_stack(callsite[i-1].imm);
		}

		// Ignore result on error else return the result.
		//
		if (any val = L->call(callsite.size(), func, self.imm); val.is_exc()) {
			return {};
		} else {
			return val;
		}
	}

	// Parses a call, returns the result.
	//
	static expression parse_call(func_scope& scope, const expression& func, const expression& self) {
		expression callsite[MAX_ARGS] = {};
		msize_t    size               = 0;

		// Allocate temporary site for result.
		//
		bc::reg tmp = scope.alloc_reg(2);

		// Collect arguments.
		//
		if (auto lit = scope.lex().opt('{')) {
			callsite[size++] = expr_table(scope);
		} else if (auto lit = scope.lex().opt(lex::token_lstr)) {
			callsite[size++] = expression(any(lit->str_val));
		} else {
			if (scope.lex().opt('!')) {
				if ((self.kind == expr::imm && self.imm == nil) || func.kind != expr::env) {
					scope.lex().error("traits should be used with :: operator.");
					return {};
				}
			}

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
					if (auto ex = expr_parse(scope); ex.kind == expr::err)
						return {};
					else
						callsite[size++] = ex;

					if (scope.lex().opt(')'))
						break;
					else if (scope.lex().check(',') == lex::token_error)
						return {};
				}
			}
		}

		// Try to constant fold the values.
		//
		if (func.kind == expr::imm && self.kind == expr::imm && func.imm.is_fn()) {
			auto  f  = func.imm.as_fn();
			auto* nf = f->ninfo;
			// TODO: is_const?
			if (nf && (nf->attr & func_attr_pure)) {
				if (std::all_of(callsite, callsite + size, [](const expression& p) { return p.kind == expr::imm; })) {
					auto res = parse_call_cxpr(scope.fn.L, std::span(callsite).subspan(0, size), f, self);
					if (res.kind != expr::err) {
						return res;
					}
				}
			}
		}

		// Create the callsite.
		//
		for (bc::reg i = (size - 1); i >= 0; i--) {
			callsite[i].push(scope);
		}
		self.push(scope);
		func.push(scope);
		scope.emit(bc::CALL, tmp, size);
		scope.reg_next = tmp + 1;
		return expression(tmp);
	}

	// Parses a format string and returns the result.
	//
	static expression parse_format(func_scope& scope) {
		expression parts[64];
		msize_t    size = 0;

		// Parse format string.
		//
		auto fmt = scope.lex().next().str_val->view();
		auto add = [&](expression e) -> bool {
			// If literal, coerce to string:
			//
			if (e.kind == expr::imm && !e.imm.is_str()) {
				e.imm = e.imm.to_string(scope.fn.L);
			}

			// Try merging with the previous instance.
			//
			if (e.kind == expr::imm && size && parts[size - 1].kind == expr::imm) {
				parts[size - 1] = any(string::concat(scope.fn.L, parts[size - 1].imm.as_str(), e.imm.as_str()));
				return true;
			}

			// Size check.
			//
			if (size == std::size(parts)) {
				scope.lex().error("format string too complex");
				return false;
			}

			// Append and return.
			//
			parts[size++] = e;
			return true;
		};
		while (!fmt.empty()) {
			// If we reached the end, push the rest as literal.
			//
			if (auto next = fmt.find_first_of("{}"); next == std::string::npos) {
				if (!add(any(string::create(scope.fn.L, fmt)))) {
					return {};
				}
				break;
			}
			// Otherwise push the leftover.
			//
			else if (next != 0) {
				if (!add(any(string::create(scope.fn.L, fmt.substr(0, next))))) {
					return {};
				}
				fmt.remove_prefix(next);
			}

			// Make sure it is properly terminated.
			//
			if (fmt.size() == 1) {
				scope.lex().error("unterminated format string");
				return {};
			}

			// Skip if escaped.
			//
			if (fmt[1] == fmt[0]) {
				if (!add(any(string::create(scope.fn.L, fmt.substr(0, 1))))) {
					return {};
				}
				fmt.remove_prefix(2);
				continue;
			}

			// Unmatched block?
			//
			if (fmt[0] == '}') {
				scope.lex().error("unmatched block within format string");
				return {};
			}

			// Parse a block expression absuing lexer buffer.
			//
			fmt.remove_prefix(1);
			auto [pi, pt, pl]  = std::tuple(scope.lex().input, scope.lex().tok, std::move(scope.lex().tok_lookahead));
			scope.fn.lex.input = fmt;
			scope.fn.lex.tok   = scope.fn.lex.scan();
			scope.fn.lex.tok_lookahead.reset();

			auto expr                 = expr_block(scope, -1, true);
			LI_ASSERT(!scope.lex().tok_lookahead);

			fmt                       = scope.lex().input;
			scope.lex().input         = pi;
			scope.lex().tok           = pt;
			scope.lex().tok_lookahead = std::move(pl);

			if (expr.kind == expr::err || !add(std::move(expr)))
				return {};
		}

		// Allocate base of concat.
		//
		auto r = scope.alloc_reg(size);
		for (msize_t i = 0; i != size; i++) {
			parts[i].to_reg(scope, r + i);
		}
		scope.emit(bc::CCAT, r, (int32_t) size);

		// Free the rest of the registers and return the result.
		//
		scope.reg_next = r + 1;
		return expression(r);
	}

	// Parses function declaration, returns the function value.
	//
	static expression parse_function(func_scope& scope, string* name) {
		// Consume the '|', '||' or '('.
		//
		msize_t line_num = scope.lex().line;
		auto id = scope.lex().next().id;

		// Create the new function.
		//
		msize_t    opt_count = 0;
		func_state new_fn{scope.fn, scope};
		new_fn.decl_name = name;
		new_fn.pc.emplace_back(); // Placeholder for VACHK.
		{
			// Collect arguments.
			//
			func_scope ns{new_fn};
			if (id != lex::token_lor) {
				auto endtk = id == '|' ? '|' : ')';
				if (!ns.lex().opt(endtk)) {
					while (true) {
						// If vararg:
						//
						if (ns.lex().opt(lex::token_dots)) {
							// Expect end of arguments, break out of the loop.
							//
							new_fn.is_vararg = true;
							if (ns.lex().check(endtk) == lex::token_error)
								return {};
							break;
						}

						// Parse argument name.
						//
						auto arg_name = ns.lex().check(lex::token_name);
						if (arg_name == lex::token_error)
							return {};

						// If optional arg:
						//
						if (ns.lex().opt('?')) {
							msize_t id = msize_t(new_fn.args.size()) + opt_count++;

							// Load vararg or nil and name it.
							//
							auto r = ns.add_local(arg_name.str_val, false);
							expression(any(number(id))).to_reg(ns, r);
							ns.emit(bc::VAGET, r, r);
						}
						// Otherwise:
						//
						else {
							// Make sure we didn't start optionals yet.
							//
							if (opt_count != 0) {
								scope.lex().error("cannot accept a required argument after an optional one.");
								return {};
							}

							// Push it in the fixed arguments list.
							//
							new_fn.args.push_back({arg_name.str_val});
							if (new_fn.args.size() > MAX_ARGS) {
								scope.lex().error("too many arguments.");
								return {};
							}

							// If there is a type annotation:
							//
							if (ns.lex().opt(':')) {
								if (!type_check_var(ns, arg_name.str_val, parse_type_name(ns))) {
									return {};
								}
							}
						}

						// If list ended break, else expect comma.
						//
						if (ns.lex().opt(endtk))
							break;
						else if (ns.lex().check(',') == lex::token_error)
							return {};
					}
				}
			}
			new_fn.pc.front().o     = bc::VACHK;
			new_fn.pc.front().a     = msize_t(new_fn.args.size());
			new_fn.pc.front().xmm() = ns.add_const(string::format(ns.fn.L, "expected at least %llu arguments", new_fn.args.size())).second.value;

			// Parse the result expression.
			//
			ns.first_scope = true;
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
		auto uv = scope.alloc_reg((msize_t) new_fn.uvalues.size());
		for (size_t n = 0; n != new_fn.uvalues.size(); n++) {
			expr_var(scope, new_fn.uvalues[n].id).to_reg(scope, uv + (bc::reg) n);
		}
		scope.emit(bc::FDUP, uv, scope.add_const(result).first, uv);

		// Free the unnecessary space and return the function by register.
		//
		scope.free_reg(uv + 1, (msize_t) new_fn.uvalues.size() - 1);
		return expression(uv);
	}

	// Parses a class declaration, returns the class value.
	//
	static expression parse_class(func_scope& scope, string* cl_name) {
		// Create a place-holder name if none given.
		//
		if (!cl_name) {
			cl_name = string::format(scope.fn.L, "<anon-class-line-%u>", scope.lex().line);
		}

		// Parse the base class.
		//
		vclass* base = nullptr;
		if (scope.lex().opt(':')) {
			auto name = expr_primary(scope, true);
			if (name.kind == expr::err) {
				return {};
			}
			if (name.kind != expr::imm || !name.imm.is_vcl()) {
				scope.lex().error("expected constant class value");
				return {};
			}
			base = name.imm.as_vcl();
		}

		// Start the block.
		//
		if (scope.lex().check('{') == lex::token_error)
			return {};
		auto ctor_line_beg = scope.lex().line;

		// Start the field iteration.
		//
		msize_t                 obj_iterator    = 0;
		msize_t                 static_iterator = 0;
		std::vector<uint8_t>    obj_data        = {};
		std::vector<uint8_t>    static_data     = {};
		std::vector<field_pair> fields          = {};

		// If there is a base class, inherit details.
		//
		if (base) {
			obj_iterator    = base->object_length;
			static_iterator = base->static_length;
			obj_data        = {base->default_space(), base->default_space() + obj_iterator};
			static_data     = {base->static_space(), base->static_space() + static_iterator};
			fields          = {base->fields().begin(), base->fields().end()};
		}

		// Start a scope for the constructor function.
		//
		func_state              ctor_fn{scope.fn, scope};
		ctor_fn.decl_name = string::format(scope.fn.L, "class %s implicit ctor", cl_name->c_str());
		{
			func_scope cscope{ctor_fn};

			// Initialize trivial structure at first register, should be the first instruction,
			// we do not know the type ID yet so this will be patched at the end of the type declaration.
			//
			bc::reg self = cscope.alloc_reg();
			cscope.emit(bc::STRIV, self, 0);

			// Allocate temporaries.
			//
			bc::reg tmpk = cscope.alloc_reg();
			bc::reg tmpv = cscope.alloc_reg();

			// Parse the fields.
			//
			while (true) {
				// Parse the name.
				//
				auto name = cscope.lex().opt(lex::token_name);
				if (!name)
					break;

				// Make sure nothing is being shadowed, insert the new field.
				//
				for (auto& f : fields) {
					if (f.key == name->str_val) {
						cscope.lex().error("definition shadows the previous value of field %s.", f.key->c_str());
						return {};
					}
				}
				auto& field = fields.emplace_back();
				uint64_t field_initv  = 0;
				field.key             = name->str_val;
				field.value.is_dyn    = false;
				field.value.is_static = false;

				// Optionally parse the type annotation.
				// - TODO: Optional values "number?"
				//
				field.value.ty = type::any;
				if (cscope.lex().opt(':')) {
					auto t = parse_type_name(cscope);
					if (std::holds_alternative<value_type>(t)) {
						field.value.ty = to_type(std::get<value_type>(t));
					} else {
						auto& ex = std::get<expression>(t);
						if (ex.kind == expr::err) {
							return {};
						} else if (ex.kind != expr::imm || !ex.imm.is_vcl()) {
							cscope.lex().error("expected constant type name.");
							return {};
						}
						field.value.ty = type(ex.imm.as_vcl()->vm_tid);
					}
				}

				// Optionally parse the default value.
				//
				if (cscope.lex().opt('=')) {
					auto ex = expr_parse(cscope);
					if (ex.kind == expr::err)
						return {};

					// If immediate:
					//
					if (ex.kind == expr::imm) {
						// Check the type.
						//
						if (ex.imm.xtype() != field.value.ty && field.value.ty != type::any) {
							auto expected = get_type_name(cscope.fn.L, field.value.ty);
							cscope.lex().error(
								 "cannot initialize field of type (%.*s) with value of type (%s).", expected.size(), expected.data(), ex.imm.type_name());
							return {};
						}

						// Set as trivial initialization.
						//
						ex.imm.store_at(&field_initv, field.value.ty);
					}
					// Otherwise, emit SSET.
					else {
						cscope.set_reg(tmpk, field.key);
						cscope.emit(bc::SSET, tmpk, ex.to_anyreg(scope), self);
					}
				} else {
					// If default initialization of complex type:
					//
					if (field.value.ty == type::arr) {
						cscope.set_reg(tmpk, field.key);
						cscope.emit(bc::ANEW, tmpv, 0);
						cscope.emit(bc::SSET, tmpk, tmpv, self);
					} else if (field.value.ty == type::tbl) {
						cscope.set_reg(tmpk, field.key);
						cscope.emit(bc::TNEW, tmpv, 0);
						cscope.emit(bc::SSET, tmpk, tmpv, self);
					} else if (field.value.ty == type::obj || field.value.ty == type::vcl) {
						auto expected = get_type_name(cscope.fn.L, field.value.ty);
						cscope.lex().error(
							 "cannot default initialize field of type %.*s.", expected.size(), expected.data());
						return {};
					}
					// Otherwise, just assign the default value as trivial init.
					//
					else {
						// Set as trivial initialization.
						//
						any::make_default(cscope.fn.L, field.value.ty).store_at(&field_initv, field.value.ty);
					}
				}

				// Determine the field offset and copy the initializer.
				//
				msize_t size       = size_of_data(field.value.ty);
				msize_t align      = align_of_data(field.value.ty);
				msize_t next_offset = ((obj_iterator + align - 1) & ~align) + size;
				field.value.offset  = next_offset - size;
				obj_iterator        = next_offset;
				obj_data.resize(obj_iterator, 0);
				memcpy(obj_data.data() + obj_iterator - size, &field_initv, size);

				// If there is no comma preceding, exit field parser.
				//
				if (!cscope.lex().opt(','))
					break;
			}

			// Return if no args passed.
			//
			cscope.emit(bc::VACNT, tmpk);
			cscope.set_reg(tmpv, number(0.0));
			cscope.emit(bc::CEQ, tmpk, tmpk, tmpv);
			auto jx = cscope.emit(bc::JS, 0, tmpk);

			// Assert that it is a table.
			//
			bc::reg tmpt = cscope.alloc_reg();
			cscope.emit(bc::VAGET, tmpt, tmpv);
			cscope.emit(bc::CTY, tmpk, tmpt, type_table);
			cscope.throw_if_not(tmpk, "implicit ctor must be passed nil or a valid table.");

			// Save nil into a permanent register, allocate another temporary for comparisons.
			//
			bc::reg tmpq    = cscope.alloc_reg();
			bc::reg tmp_nil = cscope.alloc_reg();
			cscope.set_reg(tmp_nil, nil);

			// Try assigning any relevant fields.
			//
			for (auto& f : fields) {
				// tmpv = tbl[field]
				cscope.set_reg(tmpk, f.key);
				cscope.emit(bc::TGETR, tmpv, tmpk, tmpt);

				// if tmpv == nil, skip.
				cscope.emit(bc::CEQ, tmpq, tmp_nil, tmpv);
				auto j = cscope.emit(bc::JS, 0, tmpq);

				// otherwise, emit SSET.
				//
				cscope.emit(bc::SSET, tmpk, tmpv, self);
				cscope.jump_here(j);
			}

			// Return.
			//
			cscope.jump_here(jx);
			cscope.emit(bc::RET, self);
		}

		// Until we reach the end of the declaration, only function definitions are allowed.
		// - x()
		// - t!()
		// - dyn x()
		// - dyn t!()
		while(!scope.lex().opt('}')) {
			bool dyn = scope.lex().opt(lex::token_dyn).has_value();

			// Parse the name.
			//
			auto name = scope.lex().check(lex::token_name);
			if (name == lex::token_error)
				return {};
			// bool trait = scope.lex().opt('!').has_value();

			// Make sure nothing is being shadowed, insert the new field.
			//  - Always align to 8 byte boundary.
			//
			auto it = range::find_if(fields, [&](auto& x) { return x.key == name.str_val; });
			if (it == fields.end()) {
				it = fields.emplace(fields.end());
				it->key = name.str_val;
				it->value.is_dyn    = dyn;
				it->value.is_static = true;
				it->value.offset    = static_iterator;
				it->value.ty        = type::fn;
				static_iterator += 8;
				static_data.resize(static_iterator, 0);
			} else if (!it->value.is_dyn || !it->value.is_static || it->value.ty != type::fn) {
				scope.lex().error("definition shadows the previous value of non dynamic member %s.", it->key->c_str());
				return {};
			} else {
				it->value.is_dyn    = dyn;
				it->value.is_static = true;
			}

			// Parse the function.
			//
			if (scope.lex().tok != '(') {
				scope.lex().check('(');
				return {};
			}
			auto fn = parse_function(scope, string::format(scope.fn.L, "class %s.%s", cl_name->c_str(), name.str_val->c_str()));
			if (fn.kind == expr::err) {
				return {};
			}
			if (fn.kind != expr::imm) {
				scope.lex().error("definition of the member %s is not constant.", it->key->c_str());
				return {};
			}

			// Copy the function.
			//
			fn.imm.store_at(static_data.data() + it->value.offset, it->value.ty);
		}

		// Create the VCLASS type, write the trivial data.
		//
		auto* vcl = vclass::create(scope.fn.L, cl_name, fields);
		vcl->super = base;
		LI_ASSERT(vcl->object_length == obj_iterator);
		LI_ASSERT(vcl->static_length == static_iterator);
		range::copy(obj_data, vcl->default_space());
		range::copy(static_data, vcl->static_space());

		// Fix the type in the bytecode of the implicit ctor, generate code.
		//
		ctor_fn.pc[0].xmm() = any(vcl).value;
		ctor_fn.kvalues.emplace_back(vcl);
		auto* fn = write_func(ctor_fn, ctor_line_beg, 0);

		// Mark function for aggressive inlining, assign as implicit ctor.
		//
		fn->proto->attr |= func_attr_inline;
		vcl->ctor = fn;
		return any(vcl);
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
			scope.set_reg(cc.reg, nil);
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

		// Reserve next register for break-with-value, initialize to nil.
		//
		auto result = expression{nil}.to_nextreg(scope);

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

		// Reserve next register for break-with-value, initialize to nil.
		//
		auto result = expression{nil}.to_nextreg(scope);

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
			expression i2{nil};
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
			scope.set_reg(iter_base + 4, nil);

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
			if (i2.kind != expr::imm || i2.imm != nil) {
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
				expression(any_t{0}).to_reg(iscope, iter_base);
				expression(nil).to_reg(iscope, iter_base + 3);

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
	static expression parse_try(func_scope& scope) {
		// Allocate the labels and a register for the catchpad.
		//
		auto catch_pad = scope.make_label();
		auto no_throw  = scope.make_label();
		auto pc        = std::exchange(scope.lbl_catchpad, catch_pad);

		scope.emit(bc::SETEH, catch_pad);

		// Reserve next register for block result, initialize to nil.
		//
		auto result = expression{nil}.to_nextreg(scope);

		// Parse the try block.
		//
		if (scope.lex().check('{') == lex::token_error) {
			return {};
		}
		if (expr_block(scope, result).kind == expr::err) {
			return {};
		}

		// Jump to continue.
		//
		scope.emit(bc::JMP, no_throw);

		// Set the catchpad.
		//
		scope.set_label_here(scope.lbl_catchpad);

		// Restore the label.
		//
		scope.lbl_catchpad = pc;
		scope.emit(bc::SETEH, pc);

		// Parse the catch block.
		//
		if (scope.lex().opt(lex::token_catch)) {
			func_scope iscope{scope.fn};

			if (auto errv = iscope.lex().opt(lex::token_name)) {
				auto r = iscope.add_local(errv->str_val, false);
				iscope.emit(bc::GETEX, r);
			}
			if (iscope.lex().check('{') == lex::token_error) {
				return {};
			}
			if (expr_block(iscope, result).kind == expr::err) {
				return {};
			}
		} else {
			scope.emit(bc::GETEX, result);
		}

		// Emit the result label.
		//
		scope.set_label_here(no_throw);

		// Return the result.
		//
		return result;
	}

	// Parses the code and returns it as a function instance with no arguments on success.
	//
	any load_script(vm* L, std::string_view source, std::string_view source_name, std::string_view module_name, bool is_repl) {
		// Handle UTF input.
		//
		std::string temp;
		if (util::utf_is_bom(source)) [[unlikely]] {
			temp   = util::utf_convert<char>(std::span((const uint8_t*) source.data(), source.size()));
			source = temp;
		}

		// Initialize the lexer and the function state.
		//
		lex::state lx{L, source, source_name};
		func_state fn{L, lx, is_repl};

		// If module name is not empty, create an export table.
		//
		if (!module_name.empty() && !is_repl) {
			auto* mod    = string::create(L, module_name);
			any   exists = L->modules->get(L, (any) mod);
			if (exists != nil) {
				L->error("module '%s' already exists.", mod->c_str());
				return exception_marker;
			} else {
				fn.module_table = table::create(L);
				fn.module_name  = mod;
				L->modules->set(L, (any) mod, (any) fn.module_table);
			}
		}

		// Suspend GC.
		//
		auto psuspend = std::exchange(L->gc.suspend, true);  // TODO: Not okay.

		// If REPL, use the REPL scope, otherwise create a temporary scope.
		//
		if (is_repl) {
			if (!L->repl_scope) {
				L->repl_scope = table::create(L);
			}
			fn.scope_table = L->repl_scope;
		} else {
			fn.scope_table = table::create(L);
		}

		// Parse the body.
		//
		bool ok = parse_body(fn);

		// Resume GC.
		//
		L->gc.suspend = psuspend;

		// Propagate the result.
		//
		if (!ok) {
			L->error(string::create(L, fn.lex.last_error.c_str()));
			return exception_marker;
		} else {
			return write_func(fn, 0);
		}
	}
};
