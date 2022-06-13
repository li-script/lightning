#include <lang/parser.hpp>
#include <vm/function.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <util/utf.hpp>
#include <lib/std.hpp>

#if LI_CLANG
	#pragma clang diagnostic ignored "-Wunused-function"
	#pragma clang diagnostic ignored "-Wswitch"
#endif

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
				fn.pc.push_back({bc::KIMM, 0, -1, -1});
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
		f->num_arguments = (msize_t) fn.args.size();
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

	// Reads a type name.
	//
	static value_type parse_type(func_scope& scope) {
		if (scope.lex().opt(lex::token_number)) {
			return type_number;
		} else if (scope.lex().opt(lex::token_bool)) {
			return type_bool;
		} else if (scope.lex().opt(lex::token_array)) {
			return type_array;
		} else if (scope.lex().opt(lex::token_string)) {
			return type_string;
		} else if (scope.lex().opt(lex::token_userdata)) {
			return type_userdata;
		} else if (scope.lex().opt(lex::token_function)) {
			return type_function;
		} else if (scope.lex().opt(lex::token_table)) {
			return type_table;
		} else if (scope.lex().opt(lex::token_nil)) {
			return type_nil;
		} else {
			scope.lex().error("expected type name.");
			return type_invalid;
		}
	}

	// Handles nested binary and unary operators with priorities, finally calls the expr_simple.
	//
	static expression             expr_primary(func_scope& scope);
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

		// Handle special is operator.
		//
		if (scope.lex().opt(lex::token_is)) {
			auto t = parse_type(scope);
			if (t == type_invalid) {
				return nullptr;
			} else if (lhs.kind == expr::imm) {
				out = expression(any(lhs.imm.type() == t));
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
			if (scope.fn.args[n] == name) {
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
		auto bt = scope.fn.L->modules->get(scope.fn.L, string::create(scope.fn.L, "builtin"));
		if (bt.is_tbl()) {
			bt = bt.as_tbl()->get(scope.fn.L, name);
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

	// Creates an index expression.
	//
	static expression expr_index(func_scope& scope, const expression& obj, const expression& key, bool handle_null = false) {
		if (obj.kind == expr::imm && key.kind == expr::imm) {
			if (obj.imm.is_tbl()) {
				if (obj.imm.as_tbl()->trait_freeze) {
					return obj.imm.as_tbl()->get(scope.fn.L, key.imm);
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
		// TODO: ADUP const part, dont care rn.

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
		// TODO: TDUP const part, dont care rn.

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
			return true;
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
	//
	static expression expr_primary(func_scope& scope) {
		expression base = {};

		// Variables.
		//
		if (auto& tk = scope.lex().tok; tk.id == lex::token_name) {
			base = expr_var(scope, scope.lex().next().str_val);
		}
		// Sub expressions.
		//
		else if (tk.id == '(') {
			scope.lex().next();
			base = expr_parse(scope);
			scope.lex().check(')');
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
		}
		// Functions.
		//
		else if (tk.id == lex::token_lor || tk.id == '|') {
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
				case lex::token_lstr:
				case '(': {
					base = parse_call(scope, base, nil);
					if (base.kind == expr::err)
						return {};
					break;
				}
				case lex::token_ucall: {
					scope.lex().next();
					auto ftk = scope.lex().check(lex::token_name);
					if (ftk == lex::token_error)
						return {};
					expression func = expr_var(scope, ftk.str_val, true);
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

			auto fn = parse_function(scope, name.str_val);
			if (fn.kind == expr::err) {
				return {};
			}

			expression result;
			if (is_const && fn.kind == expr::imm) {
				scope.add_local_cxpr(name.str_val, fn.imm);
				result = fn.imm;
			} else {
				bc::reg reg = scope.add_local(name.str_val, is_const);
				fn.to_reg(scope, reg);
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

				auto name = scope.lex().opt(lex::token_lstr);
				if (!name) {
					name = scope.lex().check(lex::token_name);
				}
				if (*name == lex::token_error) {
					return {};
				}

				auto mod = scope.fn.L->modules->get(scope.fn.L, name->str_val);
				if (mod == nil) {
					if (scope.fn.L->import_fn) {
						mod = scope.fn.L->import_fn(scope.fn.L, scope.lex().source_name, name->str_val->view());
					}
					if (mod == nil) {
						scope.lex().error("module '%s' not found", name->str_val->c_str());
						return {};
					}
					else if (mod.is_exc()) {
						scope.lex().error(scope.fn.L->last_ex.coerce_str(scope.fn.L)->c_str());
						return {};
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
					expression{alias.str_val}.assign(scope, mod);
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
		bool trait = false;
		if (auto lit = scope.lex().opt(lex::token_lstr)) {
			callsite[size++] = expression(any(lit->str_val));
		} else {
			if (scope.lex().opt('!')) {
				trait = true;
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

		// Handle special functions.
		//
		if (func.kind == expr::env) {
			// Handle traits.
			//
			if (trait) {
				// Arg = (0=Getter, 1=Setter).
				//
				if (size > 1) {
					scope.lex().error("trait observer should have one or no arguments.");
					return {};
				}

				// Find trait by name.
				//
				for (msize_t i = 0; i != msize_t(trait::pseudo_max); i++) {
					if (trait_names[i] == func.env->view()) {
						// Emit the opcode and return.
						//
						if (size == 0) {
							bc::reg r = self.to_nextreg(scope);
							scope.emit(bc::TRGET, r, r, i);
							return expression(r);
						} else {
							reg_sweeper _r{scope};
							scope.emit(bc::TRSET, self.to_anyreg(scope), callsite[0].to_anyreg(scope), i);
							return expression(nil);
						}
					}
				}
				scope.lex().error("unknown trait name '%s'.", func.env->c_str());
				return {};
			}
		}

		// Try to constant fold the values.
		//
		if (func.kind == expr::imm && self.kind == expr::imm && func.imm.is_fn()) {
			auto  f  = func.imm.as_fn();
			auto* nf = f->ninfo;
			// TODO: is_const?
			if (nf && nf->is_pure) {
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
		auto add = [&](const expression& e) -> bool {
			// If string literal:
			//
			if (e.kind == expr::imm && e.imm.is_str() && size) {
				// Try merging with the previous instance.
				//
				if (parts[size - 1].kind == expr::imm && parts[size - 1].imm.is_str()) {
					parts[size - 1] = any(string::concat(scope.fn.L, parts[size - 1].imm.as_str(), e.imm.as_str()));
					return true;
				}
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

			if (expr.kind == expr::err || !add(expr))
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
		// Consume the '|', '||' or ')'.
		//
		msize_t line_num = scope.lex().line;
		auto id = scope.lex().next().id;

		// Create the new function.
		//
		func_state new_fn{scope.fn, scope};
		new_fn.decl_name = name;
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
						if (new_fn.args.size() > MAX_ARGS) {
							scope.lex().error("too many arguments.");
							return {};
						}

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

		// Reserve next register for break-with-value, initialize to nil.
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

		// Reserve next register for break-with-value, initialize to nil.
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
				expression(any(opaque{.bits = 0})).to_reg(iscope, iter_base);
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
	static expression parse_try(func_scope& scope) {
		// Allocate the labels and a register for the catchpad.
		//
		auto catch_pad = scope.make_label();
		auto no_throw  = scope.make_label();
		auto pc        = std::exchange(scope.lbl_catchpad, catch_pad);

		scope.emit(bc::SETEH, catch_pad);

		// Reserve next register for block result, initialize to nil.
		//
		auto result = expression{any()}.to_nextreg(scope);

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
		if (scope.lex().check(lex::token_catch) == lex::token_error) {
			return {};
		} else {
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
			any   exists = L->modules->get(L, mod);
			if (exists != nil) {
				L->error(string::format(L, "module '%s' already exists.", mod->c_str()));
				return exception_marker;
			} else {
				fn.module_table = table::create(L);
				fn.module_name  = mod;
				L->modules->set(L, mod, fn.module_table);
			}
		}

		// If REPL, use the REPL scope, otherwise create a temporary scope and acquire GC lock.
		//
		if (is_repl) {
			if (!L->repl_scope) {
				L->repl_scope = table::create(L);
			}
			fn.scope_table = L->repl_scope;
		} else {
			fn.scope_table = table::create(L);
			fn.scope_table->acquire();
		}

		// Parse the body, release the scope table.
		//
		bool ok = parse_body(fn);
		if (!is_repl)
			fn.scope_table->release(L);

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
