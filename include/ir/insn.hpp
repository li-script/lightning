#pragma once
#include <vector>
#include <string>
#include <ir/value.hpp>
#include <util/format.hpp>
#include <util/llist.hpp>
#include <util/func.hpp>
#include <vm/function.hpp>

namespace li::ir {
	// Instruction opcodes.
	//
	enum class opcode : uint8_t {
		invalid,

		// Used to represent function context and initially locals.
		//
		load_local,
		store_local,

		// Complex types.
		//
		array_new,      // Not allowed at MIR.
		table_new,      // Not allowed at MIR.
		field_get,      // Must be typed and raw at MIR.
		field_set,      // Must be typed and raw at MIR.

		// Operators.
		//
		unop,
		binop,
		bool_and,
		bool_or,
		bool_xor,

		// Ticks the garbage collector.
		//
		gc_tick,

		// Upvalue.
		//
		uval_get,
		uval_set,

		// Casts.
		//
		assume_cast,
		coerce_bool,

		// Helpers used before transitioning to MIR.
		//
		move,
		erase_type,

		// Conditionals.
		//
		test_type,
		compare,
		select,
		phi,

		// VCALL utilities.
		//
		set_exception,
		get_exception,

		// Call types.
		//
		ccall,
		vcall, // Must be func typed at MIR.

		// Block terminators.
		//
		jmp,
		jcc,

		// Procedure terminators.
		//
		ret,
		unreachable,
	};

	// Instruction type.
	//
	struct insn : value_tag<insn> {
		// Parent block and linked list.
		//
		basic_block* parent = nullptr;
		insn*        prev   = this;
		insn*        next   = this;

		// Numbered name of the instruction value.
		//
		msize_t name = 0;

		// Opcode.
		//
		opcode opc = opcode::invalid;

		// Source/Line information.
		//
		msize_t source_bc = UINT32_MAX;

		// Traits.
		//
		uint32_t is_pure : 1     = 1;  // Always returns the same value given the same arguments (unless there was an instruction with sideeffects).
		uint32_t is_const : 1    = 0;  // On top of being pure, also doesn't break the constraints on sideffect.
		uint32_t sideffect : 1   = 0;  // Has side effects and should not be discarded if not used.
		uint32_t is_volatile : 1 = 0;  // Same as side-effect, but user specified and cannot be ignored by instruction-specific optimizers.

		// Operands.
		//
		std::vector<use<>> operands;

		// Temporary for search algorithms.
		//
		mutable uint64_t visited = 0;

		// Default construct.
		//
		insn() = default;

		// Erases the instruction from the containing block.
		//
		ref<insn> erase() {
			LI_ASSERT(parent);
			parent    = nullptr;
			util::unlink(this);

			// Return the previous parents reference.
			//
			return ref<insn>(std::in_place, this);
		}

		// Use replacement.
		//
		size_t replace_all_uses(value* with) const;
		size_t replace_all_uses_in_block(value* with, basic_block* bb = nullptr) const;
		size_t replace_all_uses_outside_block(value* with) const;

		// User enumeration.
		//
		bool for_each_user(util::function_view<bool(insn*, size_t)> cb) const;
		bool for_each_user_in_block(util::function_view<bool(insn*, size_t)> cb, basic_block* bb = nullptr) const;
		bool for_each_user_outside_block(util::function_view<bool(insn*, size_t)> cb) const;

		// Order check.
		//
		bool before(insn* with) const {
			if (with == this)
				return false;
			LI_ASSERT(parent && parent == with->parent);
			for (auto it = prev; it->parent; it = it->prev) {
				if (it == with)
					return true;
			}
			return false;
		}
		bool after(insn* with) const {
			if (with == this)
				return false;
			LI_ASSERT(parent && parent == with->parent);
			for (auto it = next; it->parent; it = it->next) {
				if (it == with)
					return true;
			}
			return false;
		}

		// Implement printer.
		//
		std::string to_string(bool expand = false) const override;

		// Copies debug info to another instance.
		//
		void copy_debug_info_to(insn* o) { o->source_bc = source_bc; }
		bool has_debug_info() const { return source_bc != bc::no_pos; }

		// Basic traits.
		//
		bool is_terminator() const { return opc >= opcode::jmp; }
		bool is_proc_terminator() const { return opc >= opcode::ret; }
		bool is_orphan() const { return !parent; }

		template<typename T>
		bool is() const {
			if constexpr (std::is_base_of_v<insn, T>) {
				if constexpr (std::is_same_v<insn, T>) {
					return true;
				} else {
					return opc == T::Opcode;
				}
			}
			return std::is_same_v<std::decay_t<T>, value>;
		}

		// Duplicates the instruction.
		//
		virtual insn* duplicate() const { return nullptr; }

		// Private copy for procedure.
		//
	  protected:
		insn(const insn&)            = default;
		insn& operator=(const insn&) = default;
	};

	// Instruction tag.
	//
	template<typename T, opcode O>
	struct insn_tag : insn {
		static constexpr opcode Opcode = O;

		// Duplicates the instruction.
		//
		insn* duplicate() const override {
			T* copy = new T(*(T*)this);
			copy->parent  = nullptr;
			copy->visited = 0;
			copy->name    = 0;
			copy->prev    = copy;
			copy->next    = copy;
			return copy;
		}
	};
	
	// Individual instructions.
	//   Missing: push,load,reset
	// // Unpacking / Repacking.
	//
	// unk  unop(const op, unk rhs)
	struct unop final : insn_tag<unop, opcode::unop> {
		void update() override {
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[0]->is<constant>());
			auto* c = operands[0]->as<constant>();
			LI_ASSERT(c->is(type::vmopr));
			LI_ASSERT(c->vmopr == operation::ANEG || c->vmopr == operation::LNOT);
			vt = type::any;
			type_try_settle(operands[1]->vt, true);
		}
		bool rec_type_check(type x) override {
			if (x > type::f64) {
				return false;
			}
			return operands[1]->type_try_settle(x);
		}
	};
	// unk  binop(const op, unk lhs, unk rhs)
	struct binop final : insn_tag<binop, opcode::binop> {
		void update() override {
			LI_ASSERT(operands.size() == 3);
			LI_ASSERT(operands[0]->is<constant>());
			auto* c = operands[0]->as<constant>();
			LI_ASSERT(c->is(type::vmopr));
			LI_ASSERT(operation::AADD <= c->vmopr && c->vmopr <= operation::APOW);

			vt = type::any;
			if (operands[1]->vt <= type::f64) {
				type_try_settle(operands[1]->vt, true);
			} else if (operands[2]->vt <= type::f64) {
				type_try_settle(operands[2]->vt, true);
			}
		}
		bool rec_type_check(type x) override {
			if (x > type::f64) {
				return false;
			}
			return operands[1]->type_try_settle(x) && operands[2]->type_try_settle(x);
		}
	};

	// i1  compare(const op, unk lhs, unk rhs)
	struct compare final : insn_tag<compare, opcode::compare> {
		void update() override {
			vt = type::i1;
			LI_ASSERT(operands.size() == 3);
			LI_ASSERT(operands[0]->is<constant>());
			auto* c = operands[0]->as<constant>();
			LI_ASSERT(c->is(type::vmopr));
			LI_ASSERT(operation::CEQ <= c->vmopr && c->vmopr <= operation::CLE);
		}
	};
	// i1   test_type(unk, const vty)
	struct test_type final : insn_tag<test_type, opcode::test_type> {
		void update() override {
			vt = type::i1;
			is_const = true; // Type of boxed types cannot change so this stays valid.
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::vty));
		}
	};
	// arr  array_new(i32)
	struct array_new final : insn_tag<array_new, opcode::array_new> {
		void update() override {
			is_pure = false;
			vt = type::arr;
			LI_ASSERT(operands.size() == 1);
			LI_ASSERT(operands[0]->is(type::i32));
		}
	};
	// tbl  table_new(i32)
	struct table_new final : insn_tag<table_new, opcode::table_new> {
		void update() override {
			is_pure = false;
			vt = type::tbl;
			LI_ASSERT(operands.size() == 1);
			LI_ASSERT(operands[0]->is(type::i32));
		}
	};
	// unk   uval_get(vfn, i32)
	struct uval_get final : insn_tag<uval_get, opcode::uval_get> {
		void update() override {
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[0]->is(type::fn));
			LI_ASSERT(operands[1]->is(type::i32));
		}
	};
	// none  gc_tick()
	struct gc_tick final : insn_tag<gc_tick, opcode::gc_tick> {
		void update() override {
			is_pure   = false;
			sideffect = true;
			LI_ASSERT(operands.size() == 0);
		}
	};
	// none   uval_set(vfn, i32, unk)
	struct uval_set final : insn_tag<uval_set, opcode::uval_set> {
		void update() override {
			is_pure   = false;
			sideffect = true;
			LI_ASSERT(operands.size() == 3);
			LI_ASSERT(operands[0]->is(type::fn));
			LI_ASSERT(operands[1]->is(type::i32));
		}
	};
	// unk    field_get(i1 raw, unk obj, unk key)
	struct field_get final : insn_tag<field_get, opcode::field_get> {
		void update() override { LI_ASSERT(operands.size() == 3); }
	};
	// none   field_set(i1 raw, unk obj, unk key, unk val)
	struct field_set final : insn_tag<field_set, opcode::field_set> {
		void update() override {
			is_pure   = false;
			sideffect = true;
			vt = type::none;
			LI_ASSERT(operands.size() == 4);
		}
	};
	// T      assume_cast(unk, const dty T)
	struct assume_cast final : insn_tag<assume_cast, opcode::assume_cast> {
		void update() override {
			is_const = true;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::dty));
			vt        = operands[1]->as<constant>()->dty;
		}
	};
	// i1     coerce_bool(unk)
	struct coerce_bool final : insn_tag<coerce_bool, opcode::coerce_bool> {
		void update() override {
			is_const = true;
			LI_ASSERT(operands.size() == 1);
			vt = type::i1;
		}
	};
	// none   ret(unk val)
	struct ret final : insn_tag<ret, opcode::ret> {
		void update() override {
			sideffect = true;
			vt = type::none;
			LI_ASSERT(operands.size() == 1);
		}
	};
	// none   unreachable()
	struct unreachable final : insn_tag<unreachable, opcode::unreachable> {
		void update() override {
			sideffect = true;
			vt        = type::none;
			LI_ASSERT(operands.empty());
		}
	};
	// none   jmp(const bb)
	struct jmp final : insn_tag<jmp, opcode::jmp> {
		void update() override {
			vt = type::none;
			LI_ASSERT(operands.size() == 1);
			LI_ASSERT(operands[0]->is<constant>() && operands[0]->is(type::bb));
		}
	};
	// none   jcc(i1 c, const bb t, const bb f)
	struct jcc final : insn_tag<jcc, opcode::jcc> {
		void update() override {
			is_pure = false;
			vt = type::none;
			LI_ASSERT(operands.size() == 3);
			LI_ASSERT(operands[0]->is(type::i1));
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::bb));
			LI_ASSERT(operands[2]->is<constant>() && operands[2]->is(type::bb));
		}
	};
	// unk  select(i1 cc, unk t, unk f)
	struct select final : insn_tag<select, opcode::select> {
		void update() override {
			is_const = true;
			LI_ASSERT(operands.size() == 3);
			LI_ASSERT(operands[0]->is(type::i1));

			vt = type::any;
			for (size_t i = 1; i != 3; i++) {
				auto& op = operands[i];
				if (op->vt != type::any) {
					type_try_settle(op->vt, true);
					break;
				}
			}
		}
		bool rec_type_check(type x) override {
			for (size_t i = 1; i != 3; i++) {
				auto& op = operands[i];
				if (!op->type_try_settle(x))
					return false;
			}
			return true;
		}
	};
	// iN   bool_and(iN a, iN b)
	struct bool_and final : insn_tag<bool_and, opcode::bool_and> {
		void update() override {
			is_const = true;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[0]->vt <= type::i64);
			LI_ASSERT(operands[1]->vt <= type::i64);
			vt = operands[0]->vt;
		}
	};
	// iN   bool_or(iN a, iN b)
	struct bool_or final : insn_tag<bool_or, opcode::bool_or> {
		void update() override {
			is_const = true;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[0]->vt <= type::i64);
			LI_ASSERT(operands[1]->vt <= type::i64);
			vt = operands[0]->vt;
		}
	};
	// iN   bool_xor(iN a, iN b)
	struct bool_xor final : insn_tag<bool_xor, opcode::bool_xor> {
		void update() override {
			is_const = true;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[0]->vt <= type::i64);
			LI_ASSERT(operands[1]->vt <= type::i64);
			vt = operands[0]->vt;
		}
	};
	// unk   phi(unk...)
	struct phi final : insn_tag<phi, opcode::phi> {
		void update() override {
			is_const = true;

			vt = type::none;
			for (auto& op : operands) {
				if (op->vt != type::any) {
					vt = type::any;
					type_try_settle(op->vt, true);
					break;
				} else {
					vt = op->vt;
				}
			}
		}
		bool rec_type_check(type x) override {
			for (auto& op : operands) {
				if (!op->type_try_settle(x))
					return false;
			}
			return true;
		}
	};
	// unk  load_local(i32)
	struct load_local final : insn_tag<load_local, opcode::load_local> {
		void update() override {
			is_pure = true;
			vt = type::any;
			LI_ASSERT(operands.size() == 1);
			LI_ASSERT(operands[0]->is<constant>() && operands[0]->is(type::i32));
		}
	};
	// none store_local(i32, unk)
	struct store_local final : insn_tag<store_local, opcode::store_local> {
		void update() override {
			is_pure   = false;
			sideffect = true;
			vt        = type::none;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[0]->is<constant>() && operands[0]->is(type::i32));
		}
	};
	// T    move(T x)
	struct move final : insn_tag<move, opcode::move> {
		void update() override {
			is_const  = true;
			LI_ASSERT(operands.size() == 1);
			vt = operands[0]->vt;
		}
		bool rec_type_check(type x) override {
			return operands[0]->type_try_settle(x);
		}
	};
	// unk  erase_type(T x)
	struct erase_type final : insn_tag<erase_type, opcode::erase_type> {
		void update() override {
			is_const = true;
			LI_ASSERT(operands.size() == 1);
			vt = type::any;
		}
	};
	// T ccall(nfni target, i32 overloadid, unk... args)
	struct ccall final : insn_tag<ccall, opcode::ccall> {
		void update() override {
			LI_ASSERT(operands.size() >= 2);
			LI_ASSERT(operands[0]->is<constant>() && operands[0]->is(type::nfni));
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::i32));
			auto* nf   = operands[0]->as<constant>()->nfni;
			auto  ovl  = operands[1]->as<constant>()->i32;
			vt         = nf->overloads[ovl].ret;
			is_pure    = nf->attr & func_attr_pure;
			is_const   = nf->attr & func_attr_const;
			sideffect  = nf->attr & func_attr_sideeffect;
		}
	};
	// none set_exception(unk)
	struct set_exception final : insn_tag<set_exception, opcode::set_exception> {
		void update() override {
			is_pure   = false;
			sideffect = true;
			vt      = type::none;
			LI_ASSERT(operands.size() == 1);
		}
	};
	// unk  get_exception()
	struct get_exception final : insn_tag<get_exception, opcode::get_exception> {
		void update() override {
			is_pure   = false;
			vt        = type::any;
			LI_ASSERT(operands.size() == 0);
		}
	};
	// unk vcall(unk target, unk self, unk... args)
	struct vcall final : insn_tag<vcall, opcode::vcall> {
		void update() override {
			is_pure   = false;
			sideffect = true;
			vt        = type::any;
			LI_ASSERT(operands.size() >= 2);
			if (operands[0]->is<constant>()) {
				auto* c = operands[0]->as<constant>();
				LI_ASSERT(c->is(type::fn));
				if (auto* nf = c->fn->ninfo) {
					is_pure   = nf->attr & func_attr_pure;
					is_const  = nf->attr & func_attr_const;
					sideffect = nf->attr & func_attr_sideeffect;
					// vt        = nf->ret;
				}
			}
		}
	};
};