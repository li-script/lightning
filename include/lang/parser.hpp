#pragma once
#include <vm/function.hpp>
#include <lang/lexer.hpp>
#include <lang/operator.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <vector>
#include <tuple>
#include <util/format.hpp>

namespace li {
	// Local value descriptor.
	//
	struct local_state {
		string* id       = nullptr;  // Local name.
		bool    is_const = false;    // Set if declared as const.
		bc::reg reg      = -1;       // Register mapping to it if any.
		any     cxpr     = nil;     // Constant expression if any.
	};

	// Label magic flag.
	//
	static constexpr uint32_t label_flag = 0x40000000;

	// Argument slot.
	//
	struct arg_slot {
		string* name;
	};

	// Function parser state.
	//
	struct func_scope;
	struct func_state {
		vm*                      L;                       // VM state.
		lex::state&              lex;                     // Lexer state.
		func_scope*              enclosing  = nullptr;    // Enclosing function's scope.
		std::vector<local_state> uvalues    = {};         // Upvalues mapped by name.
		func_scope*              scope      = nullptr;    // Current scope.
		std::vector<any>         kvalues    = {};         // Constant pool.
		bc::reg                  max_reg_id = 1;          // Maximum register ID used.
		std::vector<arg_slot>    args       = {};         // Arguments.
		bool                     is_vararg  = false;      // Set if vararg.
		std::vector<bc::insn>    pc         = {};         // Bytecode generated.
		bool                     is_repl    = false;      // Disables locals.
		string*                  decl_name  = nullptr;    // Name.
		std::vector<line_info>   line_table = {};         // Record for each time a line changed.
		uint32_t                 last_line  = 0;          //
		uint32_t                 last_lexed_line;         //
		table*                   scope_table  = nullptr;  // Table holding scope variables.
		table*                   module_table = nullptr;  // Table holding exports.
		string*                  module_name  = nullptr;  //

		// Labels.
		//
		uint32_t                                 next_label = label_flag;  // Next label id.
		std::vector<std::pair<bc::rel, bc::pos>> label_map  = {};          // Maps label id to position.

		// Constructors.
		//
		func_state(vm* L, lex::state& lex, bool is_repl)
			: L(L), lex(lex), last_lexed_line(lex.line) {}
		func_state(func_state& parent, func_scope& enclosing)
			: L(parent.L), lex(parent.lex), last_lexed_line(lex.line), enclosing(&enclosing),
			scope_table(parent.scope_table), module_table(parent.module_table) {}

		// Syncs line-table with instruction stream.
		//
		void synclines(bc::pos ip) {
			if (last_line != last_lexed_line) {
				int32_t delta = last_lexed_line - last_line;
				LI_ASSERT(delta > 0);
				line_table.push_back({ip, (bc::pos) delta});
				last_line = last_lexed_line;
			}
		}
	};

	struct expression;

	// Local scope state.
	//
	struct func_scope {
		func_state&              fn;                    // Function that scope belongs to.
		func_scope*              prev;                  // Outer scope.
		bc::reg                  reg_next     = 0;      // Next free register.
		std::vector<local_state> locals       = {};     // Locals declared in this scope.
		bc::rel                  lbl_continue = 0;      // Labels.
		bc::rel                  lbl_break    = 0;      //
		bc::rel                  lbl_catchpad = 0;      //
		bool                     first_scope  = false;  // Set if first scope in function decl.

		// Emits an instruction and returns the position in stream.
		//
		bc::pos emit(bc::opcode o, bc::reg a = 0, bc::reg b = 0, bc::reg c = 0) {
			fn.pc.emplace_back(bc::insn{o, a, b, c});
			bc::pos ip = bc::pos(fn.pc.size() - 1);
			fn.synclines(ip);
			return ip;
		}
		bc::pos emitx(bc::opcode o, bc::reg a, uint64_t xmm) {
			fn.pc.emplace_back(bc::insn{o, a}).xmm() = xmm;
			bc::pos ip                               = bc::pos(fn.pc.size() - 1);
			fn.synclines(ip);
			return ip;
		}

		// Quick throw helper.
		//
		void throw_if(const expression& cc, string* msg, bool inv = false);

		template<typename... Tx>
		void throw_if(const expression& cc, const char* msg, Tx... args) {
			throw_if(cc, string::format(fn.L, msg, args...), false);
		}
		template<typename... Tx>
		void throw_if_not(const expression& cc, const char* msg, Tx... args) {
			throw_if(cc, string::format(fn.L, msg, args...), true);
		}

		// Reserves a label identifier.
		//
		bc::rel make_label() { return ++fn.next_label; }

		// Sets a label.
		//
		void set_label_here(bc::rel l) { fn.label_map.emplace_back(l, bc::pos(fn.pc.size())); }

		// Helper for fixing jump targets.
		//
		void jump_here(bc::pos br) { fn.pc[br].a = msize_t(fn.pc.size()) - (br + 1); }

		// Gets the lexer.
		//
		lex::state& lex() {
			fn.last_lexed_line = fn.lex.line;
			return fn.lex;
		}

		// Inserts a new local variable.
		//
		void    add_local_cxpr(string* name, any val) { locals.push_back({name, true, -1, val}); }
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
			if (v.is_gc()) {
				emitx(bc::KIMM, r, add_const(v).second.value);
			} else {
				emitx(bc::KIMM, r, v.value);
			}
		}

		// Allocates/frees registers.
		//
		bc::reg alloc_reg(msize_t n = 1) {
			bc::reg r = reg_next;
			reg_next += n;
			fn.max_reg_id = std::max(fn.max_reg_id, bc::reg(r + n - 1));
			return r;
		}
		void free_reg(bc::reg r, msize_t n = 1) {
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
				lbl_catchpad = prev->lbl_catchpad;
			}
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
		uvl,  // upvalue
		env,  // environment
		exp,  // export
		idx,  // index into local with another local
	};
	struct upvalue_t {};
	struct export_t {};
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
			string* env;
			string* exp;
		};

		// Default constructor maps to error.
		//
		expression() {}

		// Constructed by value.
		//
		expression(bc::reg u, bool freeze = false) : kind(expr::reg), freeze(freeze), reg(u) {}
		expression(upvalue_t, bc::reg u, bool freeze = false) : kind(expr::uvl), freeze(freeze), reg(u) {}

		expression(any k) : kind(expr::imm), imm(k) {}

		expression(string* g) : kind(expr::env), env(g) {}
		expression(export_t, string* g) : kind(expr::exp), exp(g) {}
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
			switch (kind) {
				case expr::err:
					util::abort("unhandled error expression");
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
				case expr::exp:
				case expr::env: {
					auto tbl = scope.fn.scope_table;
					if (kind == expr::exp && scope.fn.module_table)
						tbl = scope.fn.module_table;
					auto tmp = scope.alloc_reg(2);
					scope.set_reg(tmp, any(tbl));
					scope.set_reg(tmp + 1, any(env));
					scope.emit(bc::TGETR, r, tmp + 1, tmp);
					scope.free_reg(tmp, 2);
					return;
				}
				case expr::idx:
					scope.emit(bc::TGET, r, idx.field, idx.table);
					return;
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
				case expr::err:
					util::abort("unhandled error expression");
				case expr::reg:
					scope.emit(bc::PUSHR, reg);
					return;
				case expr::imm:
					if (imm.is_gc()) {
						scope.emitx(bc::PUSHI, 0, scope.add_const(imm).second.value);
					} else {
						scope.emitx(bc::PUSHI, 0, imm.value);
					}
					return;
				case expr::uvl:
				case expr::idx:
				case expr::env:
				case expr::exp:
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
				case expr::exp:
				case expr::env: {
					auto tbl = scope.fn.scope_table;
					if (kind == expr::exp && scope.fn.module_table)
						tbl = scope.fn.module_table;
					auto tmp = scope.alloc_reg(2);
					auto val = value.to_anyreg(scope);
					scope.set_reg(tmp, any(tbl));
					scope.set_reg(tmp + 1, any(env));
					scope.emit(bc::TSETR, tmp+1, val, tmp);
					scope.reg_next = tmp;
					return;
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
						if (reg == FRAME_SELF) {
							printf(LI_GRN "self" LI_DEF);
						} else if (reg == FRAME_TARGET) {
							printf(LI_GRN "$F" LI_DEF);
						} else {
							printf(LI_YLW "a%u" LI_DEF, (uint32_t) - (reg + FRAME_SIZE));
						}
					} else {
						printf(LI_RED "r%u" LI_DEF, (uint32_t) reg);
					}
					break;
				case expr::uvl:
					printf(LI_GRN "u%u" LI_DEF, (uint32_t) reg);
					break;
				case expr::env:
					printf(LI_PRP "ENV[%s]" LI_DEF, env->c_str());
					break;
				case expr::exp:
					printf(LI_PRP "EXP[%s]" LI_DEF, exp->c_str());
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
	expression emit_unop(func_scope& scope, bc::opcode op, const expression& rhs);
	expression emit_binop(func_scope& scope, const expression& lhs, bc::opcode op, const expression& rhs);

	// Parses the code and returns it as a function instance with no arguments on success.
	//
	any load_script(vm* L, std::string_view source, std::string_view source_name = "", std::string_view module_name = "", bool is_repl = false);
};