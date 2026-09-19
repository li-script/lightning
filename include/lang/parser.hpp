#pragma once
#include <lang/lexer.hpp>
#include <lang/operator.hpp>
#include <lang/typespec.hpp>
#include <tuple>
#include <util/format.hpp>
#include <vector>
#include <vm/function.hpp>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>

namespace li {
	struct vclass;
	struct generic_template;

	// Owns allocation results that are only needed while a compilation is in
	// progress. Values promoted into prototypes, classes, or VM tables acquire
	// their own references before these roots are released.
	//
	struct parser_roots {
		vm*              L;
		std::vector<any> values = {};

		explicit parser_roots(vm* L) : L(L) {}
		parser_roots(const parser_roots&)            = delete;
		parser_roots& operator=(const parser_roots&) = delete;

		void adopt(any value) {
			if (value.is_gc())
				values.push_back(value);
		}

		~parser_roots() {
			for (auto it = values.rbegin(); it != values.rend(); ++it)
				rc::release(L, *it);
		}
	};

	// Local value descriptor.
	//
	struct local_state {
		string*          id          = nullptr;  // Local name.
		bool             is_const    = false;    // Set if declared as const.
		bc::reg          reg         = -1;       // Register mapping to it if any.
		any              cxpr        = nil;      // Constant expression if any.
		strict::typespec static_type = strict::typespec::any();
	};

	// Label magic flag.
	//
	static constexpr uint32_t label_flag = 0x40000000;

	// Argument slot.
	//
	struct arg_slot {
		string*          name;
		strict::typespec static_type = strict::typespec::any();
	};
	struct type_alias_entry {
		string*          name;
		strict::typespec value;
	};
	struct strict_field_binding {
		string*          name;
		strict::typespec value;
		int32_t          union_group = -1;
		bool             is_atomic   = false;
	};
	struct variadic_type_alias_entry {
		string*                       name;
		std::vector<strict::typespec> values;
	};
	struct union_write_state {
		vclass* declared = nullptr;
		int32_t group    = -1;
		string* field    = nullptr;
	};
	struct nominal_type_binding {
		vclass*                           declared = nullptr;
		std::vector<strict_field_binding> fields;
	};

	enum class generic_instantiation_status : uint8_t {
		ok,
		cache_ok,
		failed,
	};
	struct generic_parameter {
		string*                         name       = nullptr;
		std::optional<strict::typespec> constraint = std::nullopt;
		bool                            variadic   = false;
	};
	struct generic_candidate {
		lex::state                     source;
		std::vector<generic_parameter> parameters;
		std::vector<strict::typespec>  parameter_patterns;
		bool                           variadic_call_pattern = false;
		table*                         attributes            = nullptr;
		bool                           class_template        = false;
		bool                           value_semantics       = false;
		string*                        class_decl_name       = nullptr;
		uint64_t                       class_decl_identity   = 0;

		generic_candidate(lex::state source) : source(std::move(source)) {}
	};
	struct generic_instantiation_record {
		std::vector<strict::typespec> types;
		generic_instantiation_status  status = generic_instantiation_status::failed;
		any                           value  = nil;
		std::string                   message;
		size_t                        candidate_index = 0;
	};
	struct generic_class_in_progress {
		std::vector<strict::typespec> types;
		vclass*                       forward = nullptr;
	};
	struct generic_template {
		vm*                                       L;
		string*                                   name;
		std::vector<generic_candidate>            candidates;
		std::vector<generic_instantiation_record> instantiations;
		std::vector<generic_class_in_progress>    classes_in_progress;
		std::vector<vclass*>                      recursive_forwards;
		function*                                 declaration_value = nullptr;
		vclass*                                   declaration_class = nullptr;

		generic_template(vm* L, string* name) : L(L), name(name) { rc::retain(name); }
		~generic_template();
	};

	// Function parser state.
	//
	struct func_scope;
	struct func_state {
		vm*                                    L;                                 // VM state.
		lex::state&                            lex;                               // Lexer state.
		parser_roots&                          roots;                             // Compilation-lifetime owning roots.
		func_scope*                            enclosing              = nullptr;  // Enclosing function's scope.
		std::vector<local_state>               uvalues                = {};       // Upvalues mapped by name.
		func_scope*                            scope                  = nullptr;  // Current scope.
		std::vector<any>                       kvalues                = {};       // Constant pool.
		bc::reg                                max_reg_id             = 1;        // Maximum register ID used.
		std::vector<arg_slot>                  args                   = {};       // Required positional arguments.
		std::vector<strict::typespec>          parameter_types        = {};       // All positional parameter types.
		std::vector<type_alias_entry>          type_aliases           = {};       // Lexically available strict aliases.
		std::vector<variadic_type_alias_entry> variadic_type_aliases  = {};       // Bound generic type packs.
		std::vector<nominal_type_binding>      nominal_types          = {};       // Parser-only nominal member types.
		std::vector<union_write_state>         union_writes           = {};       // Most recent strict union writes per function.
		generic_template*                      active_generic         = nullptr;  // Generic currently being reparsed.
		msize_t                                parameter_count        = 0;        // Required and optional positional arguments.
		bool                                   is_vararg              = false;    // Set if vararg.
		string*                                vararg_name            = nullptr;  // Optional named vararg view.
		uint32_t                               defer_body_depth       = 0;        // Deferred body currently being emitted.
		std::vector<bc::insn>                  pc                     = {};       // Bytecode generated.
		bool                                   is_repl                = false;    // Root declarations use the persistent REPL environment.
		bool                                   mutable_environment    = false;    // Unresolved bindings may be assigned.
		uint32_t                               continuation_depth     = 0;        // ()/[]/{} initializer nesting.
		uint32_t                               lexical_lock_depth     = 0;        // Direct yield is forbidden while parsing a lock body.
		uint32_t                               atomic_parse_depth     = 0;        // Enables restricted numeric-loop lowering.
		uint32_t                               block_terminator_depth = 0;        // A following '{' terminates the current expression.
		bool                                   disable_jit            = false;    // Internal restricted plans are never invoked or JITed.
		bool                                   strict_mode            = false;    // Static tier is active in this function.
		bool                                   class_strict_mode      = false;    // Enclosing class propagates strict member mode.
		bool                                   static_require_failed  = false;    // Candidate was removed by static require.
		std::string                            static_require_message = {};       // Innermost failed requirement.
		lex::token_value                       static_require_at      = {};       // Requirement source token.
		bool                                   has_return_guard       = false;    // Function has an explicit return annotation.
		bool                                   return_nullable        = false;    // Annotated return additionally accepts nil.
		value_type                             return_type            = type_invalid;
		vclass*                                return_class           = nullptr;  // Borrowed compile-time class guard.
		strict::typespec                       return_spec            = strict::typespec::any();
		std::optional<strict::typespec>        inferred_return        = std::nullopt;
		uint64_t                               return_class_identity  = 0;        // Cycle-free guard for the class currently being declared.
		string*                                class_decl_name        = nullptr;  // Borrowed contextual Self/class spelling.
		uint64_t                               class_decl_identity    = 0;
		std::vector<strict_field_binding>*     class_field_types      = nullptr;  // Borrowed current declaration environment.
		string*                                decl_name              = nullptr;  // Name.
		std::vector<line_info>                 line_table             = {};       // Record for each time a line changed.
		uint32_t                               last_line              = 0;        //
		uint32_t                               last_lexed_line;                   //
		table*                                 scope_table  = nullptr;            // Table holding scope variables.
		table*                                 module_table = nullptr;            // Table holding exports.
		table*                                 type_table   = nullptr;            // Private forward nominal-type registry.
		table*                                 attributes   = nullptr;            // Pending immutable declaration/file attributes.
		string*                                module_name  = nullptr;            //

		// Labels.
		//
		uint32_t                                 next_label = label_flag;  // Next label id.
		std::vector<std::pair<bc::rel, bc::pos>> label_map  = {};          // Maps label id to position.

		// Constructors.
		//
		func_state(vm* L, lex::state& lex, parser_roots& roots, bool is_repl)
			 : L(L), lex(lex), roots(roots), is_repl(is_repl), mutable_environment(is_repl), last_lexed_line(lex.line) {}
		func_state(func_state& parent, func_scope& enclosing)
			 : L(parent.L),
				lex(parent.lex),
				roots(parent.roots),
				enclosing(&enclosing),
				nominal_types(parent.nominal_types),
				active_generic(parent.active_generic),
				mutable_environment(parent.mutable_environment),
				strict_mode(parent.strict_mode || parent.class_strict_mode),
				class_strict_mode(parent.class_strict_mode),
				class_decl_name(parent.class_decl_name),
				class_decl_identity(parent.class_decl_identity),
				class_field_types(parent.class_field_types),
				last_lexed_line(lex.line),
				scope_table(parent.scope_table),
				module_table(parent.module_table),
				type_table(parent.type_table),
				module_name(parent.module_name) {
			for (const auto& alias : parent.type_aliases) {
				rc::retain(alias.name);
				type_aliases.push_back(alias);
			}
			for (const auto& alias : parent.variadic_type_aliases) {
				rc::retain(alias.name);
				variadic_type_aliases.push_back(alias);
			}
			rc::retain(scope_table);
			rc::retain(module_table);
			rc::retain(type_table);
			rc::retain(module_name);
		}
		func_state(const func_state&)            = delete;
		func_state& operator=(const func_state&) = delete;
		~func_state() {
			for (auto it = kvalues.rbegin(); it != kvalues.rend(); ++it)
				rc::release(L, *it);
			for (auto it = uvalues.rbegin(); it != uvalues.rend(); ++it)
				rc::release(L, it->id);
			for (auto it = args.rbegin(); it != args.rend(); ++it)
				rc::release(L, it->name);
			for (auto it = type_aliases.rbegin(); it != type_aliases.rend(); ++it)
				rc::release(L, it->name);
			for (auto it = variadic_type_aliases.rbegin(); it != variadic_type_aliases.rend(); ++it)
				rc::release(L, it->name);
			rc::release(L, vararg_name);
			rc::release(L, decl_name);
			rc::release(L, scope_table);
			rc::release(L, module_table);
			rc::release(L, type_table);
			rc::release(L, attributes);
			rc::release(L, module_name);
		}

		template<typename T>
		T* own(T* value) {
			roots.adopt(any(value));
			return value;
		}
		any own(any value) {
			roots.adopt(value);
			return value;
		}
		void set_decl_name(string* value) { rc::replace(L, decl_name, value); }
		void set_vararg_name(string* value) { rc::replace(L, vararg_name, value); }
		void set_scope_table(table* value) { rc::replace(L, scope_table, value); }
		void set_module_table(table* value) { rc::replace(L, module_table, value); }
		void set_module_name(string* value) { rc::replace(L, module_name, value); }
		void set_attributes(table* value) { rc::replace(L, attributes, value); }
		void adopt_scope_table(table* value) {
			rc::release(L, scope_table);
			scope_table = value;
		}
		void adopt_module_table(table* value) {
			rc::release(L, module_table);
			module_table = value;
		}
		void adopt_type_table(table* value) {
			rc::release(L, type_table);
			type_table = value;
		}

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
		func_state&              fn;                        // Function that scope belongs to.
		func_scope*              prev;                      // Outer scope.
		bc::reg                  reg_next         = 0;      // Next free register.
		std::vector<local_state> locals           = {};     // Locals declared in this scope.
		bc::rel                  lbl_continue     = 0;      // Nearest loop's continue target.
		bc::rel                  lbl_break        = 0;      // Nearest loop's break target.
		bc::rel                  lbl_leave        = 0;      // Nearest block expression's leave target.
		bc::rel                  lbl_catchpad     = 0;      // Normal exception handler target.
		bc::rel                  lbl_cleanup      = 0;      // Forced-unwind cleanup target.
		bool                     catchpad_cleanup = false;  // Normal target is cleanup-only.
		bc::reg                  reg_break        = -1;     // Result register owned by lbl_break.
		bc::reg                  reg_leave        = -1;     // Result register owned by lbl_leave.
		func_scope*              owner_break      = nullptr;
		func_scope*              owner_continue   = nullptr;
		func_scope*              owner_leave      = nullptr;
		std::vector<bc::rel>     cleanup_entries  = {};  // Deferred bodies, oldest to newest.
		bc::rel                  cleanup_dispatch = 0;
		bc::reg                  cleanup_action   = -1;
		bc::reg                  cleanup_value    = -1;     // Owned return value or pending exception.
		bool                     first_scope      = false;  // Set if first scope in function decl.

		// Emits an instruction and returns the position in stream.
		//
		bc::pos emit(bc::opcode o, bc::reg a = 0, bc::reg b = 0, bc::reg c = 0) {
			fn.pc.emplace_back(bc::insn{o, a, b, c});
			bc::pos ip = bc::pos(fn.pc.size() - 1);
			fn.synclines(ip);
			return ip;
		}
		bc::pos emitx(bc::opcode o, bc::reg a, uint64_t xmm) {
			fn.pc.emplace_back(bc::insn{o, a});
			fn.pc.back().set_xmm(xmm);
			bc::pos ip = bc::pos(fn.pc.size() - 1);
			fn.synclines(ip);
			return ip;
		}

		// Quick throw helper.
		//
		void throw_if(const expression& cc, string* msg, bool inv = false);

		template<typename... Tx>
		void throw_if(const expression& cc, const char* msg, Tx... args) {
			throw_if(cc, fn.own(string::format(fn.L, msg, args...)), false);
		}
		template<typename... Tx>
		void throw_if_not(const expression& cc, const char* msg, Tx... args) {
			throw_if(cc, fn.own(string::format(fn.L, msg, args...)), true);
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

		// Inserts a new local variable. Parser bindings own their lexer-provided
		// names, and constant bindings own their values.
		//
		void add_local_cxpr(string* name, any val, strict::typespec static_type = strict::typespec::any()) {
			rc::retain(name);
			rc::retain(val);
			locals.push_back({name, true, -1, val, std::move(static_type)});
		}
		void add_local_at(string* name, bool is_const, bc::reg reg, strict::typespec static_type = strict::typespec::any()) {
			rc::retain(name);
			locals.push_back({name, is_const, reg, nil, std::move(static_type)});
		}
		bc::reg add_local(string* name, bool is_const, strict::typespec static_type = strict::typespec::any()) {
			auto r = alloc_reg();
			add_local_at(name, is_const, r, std::move(static_type));
			return r;
		}
		void remove_last_local() {
			LI_ASSERT(!locals.empty());
			auto& local = locals.back();
			rc::release(fn.L, local.id);
			if (local.reg == -1)
				rc::release(fn.L, local.cxpr);
			locals.pop_back();
		}
		void clear_locals() {
			for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
				rc::release(fn.L, it->id);
				if (it->reg == -1)
					rc::release(fn.L, it->cxpr);
			}
			locals.clear();
		}

		// Inserts a new constant into the pool.
		//
		std::pair<bc::reg, any> add_const(any c) {
			for (size_t i = 0; i != fn.kvalues.size(); i++) {
				if (fn.kvalues[i] == c) {
					return {(bc::reg) i, fn.kvalues[i]};
				}
			}
			rc::retain(c);
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
		void clear_regs(bc::reg r, msize_t n = 1) {
			for (bc::reg reg = r; reg != r + n; ++reg)
				set_reg(reg, nil);
		}
		void free_reg(bc::reg r, msize_t n = 1) {
			LI_ASSERT((r + n) == reg_next);
			clear_regs(r, n);
			reg_next -= n;
		}
		void discard_regs(bc::reg next) {
			LI_ASSERT(next <= reg_next);
			if (next != reg_next)
				free_reg(next, reg_next - next);
		}
		bool is_local_reg(bc::reg reg) const {
			for (auto* scope = this; scope; scope = scope->prev) {
				for (const auto& local : scope->locals) {
					if (local.reg == reg)
						return true;
				}
			}
			return false;
		}

		// Construction and destruction in RAII pattern, no copy.
		//
		func_scope(func_state& fn) : fn(fn), prev(fn.scope) {
			fn.scope = this;
			if (prev) {
				reg_next         = prev->reg_next;
				lbl_break        = prev->lbl_break;
				lbl_continue     = prev->lbl_continue;
				lbl_leave        = prev->lbl_leave;
				lbl_catchpad     = prev->lbl_catchpad;
				lbl_cleanup      = prev->lbl_cleanup;
				catchpad_cleanup = prev->catchpad_cleanup;
				reg_break        = prev->reg_break;
				reg_leave        = prev->reg_leave;
				owner_break      = prev->owner_break;
				owner_continue   = prev->owner_continue;
				owner_leave      = prev->owner_leave;
			}
		}
		func_scope(const func_scope&)            = delete;
		func_scope& operator=(const func_scope&) = delete;
		~func_scope() {
			clear_locals();
			fn.scope = prev;
		}
	};

	// Reg-sweep helper, clears discarded frame slots and restores the register
	// counter on destruction.
	//
	struct reg_sweeper {
		func_scope& s;
		bc::reg     v;
		reg_sweeper(func_scope& s) : s(s), v(s.reg_next) {}
		~reg_sweeper() { s.discard_regs(v); }
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
		expr              kind : 7             = expr::err;
		uint8_t           freeze : 1           = false;
		strict::typespec  static_type          = strict::typespec::any();
		bool              is_type_literal      = false;
		bool              fresh_struct_value   = false;
		generic_template* generic_owner        = nullptr;
		vclass*           union_owner          = nullptr;
		string*           union_field          = nullptr;
		int32_t           union_group          = -1;
		lex::token_value  union_read_at        = {};  // Field-name token of a union read, for diagnostics.
		bool              struct_element_field = false;
		bool              atomic_field         = false;
		bc::reg           writeback_table      = -1;
		bc::reg           writeback_index      = -1;
		bc::reg           writeback_value      = -1;

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
		expression(bc::reg u, bool freeze = false, strict::typespec type = strict::typespec::any())
			 : kind(expr::reg), freeze(freeze), static_type(std::move(type)), reg(u) {}
		expression(upvalue_t, bc::reg u, bool freeze = false, strict::typespec type = strict::typespec::any())
			 : kind(expr::uvl), freeze(freeze), static_type(std::move(type)), reg(u) {}

		expression(any k) : kind(expr::imm), static_type(strict::parse_from_dynamic(k.type())), imm(k) {}
		expression(any k, strict::typespec type) : kind(expr::imm), static_type(std::move(type)), imm(k) {}

		expression(string* g) : kind(expr::env), env(g) {}
		expression(export_t, string* g) : kind(expr::exp), exp(g) {}
		expression(bc::reg tbl, bc::reg field, strict::typespec type = strict::typespec::any())
			 : kind(expr::idx), static_type(std::move(type)), idx{tbl, field} {}

		// The discriminant and every union alternative are trivially copyable.
		//
		expression(const expression&)            = default;
		expression& operator=(const expression&) = default;

		// Returns true if the expression if of type lvalue and can be assigned to.
		//
		bool is_lvalue() const { return kind >= expr::reg; }

		// Returns true if the expression is a dispatched value.
		//
		bool is_value() const { return kind == expr::imm || kind == expr::reg; }

		// Validates a strict union read against the most recent write in this function.
		//
		bool validate_union_read(func_scope& scope) const {
			if (!scope.fn.strict_mode || union_group < 0)
				return true;
			for (const auto& state : scope.fn.union_writes) {
				if (state.declared == union_owner && state.group == union_group) {
					if (state.field == union_field || (state.field && union_field && state.field->view() == union_field->view()))
						return true;
					break;
				}
			}
			if (union_read_at.source_line)
				scope.lex().error_at(union_read_at, "union field '%s' was not the most recently assigned field in this function", union_field->c_str());
			else
				scope.lex().error("union field '%s' was not the most recently assigned field in this function", union_field->c_str());
			return false;
		}

		// Stores the expression value to the specified register.
		//
		void to_reg(func_scope& scope, bc::reg r) const {
			if (!validate_union_read(scope)) {
				scope.set_reg(r, nil);
				return;
			}
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
			if (!validate_union_read(scope))
				return;
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

			if (scope.fn.strict_mode && union_group >= 0) {
				bool found = false;
				for (auto& state : scope.fn.union_writes) {
					if (state.declared == union_owner && state.group == union_group) {
						state.field = union_field;
						found       = true;
						break;
					}
				}
				if (!found)
					scope.fn.union_writes.push_back({union_owner, union_group, union_field});
			}

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
					scope.emit(bc::TSETR, tmp + 1, val, tmp);
					scope.discard_regs(tmp);
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
					if (struct_element_field)
						scope.emit(bc::TSET, writeback_index, writeback_value, writeback_table);
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
							printf(LI_YLW "a%u" LI_DEF, (uint32_t) -(reg + FRAME_SIZE));
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