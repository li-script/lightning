#pragma once
#include <ir/value.hpp>
#include <string>
#include <util/format.hpp>
#include <util/func.hpp>
#include <util/llist.hpp>
#include <vector>
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
		array_new,  // Not allowed at MIR.
		table_new,  // Not allowed at MIR.
		field_get,
		field_set,
		struct_array_get,
		struct_array_class_test,
		struct_array_bounds_test,
		struct_array_field_load,
		class_new,
		class_is,
		iter_next,

		// Operators.
		//
		unop,
		binop,
		bool_and,
		bool_or,
		bool_xor,

		// Runtime coordination.
		//
		safepoint,
		va_count,
		va_get,
		extract,

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
		keep_alive,
		retain,
		release,

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
		vcall,  // Must be func typed at MIR.
		osr_load,

		// Block terminators.
		//
		jmp,
		jcc,

		// Procedure terminators.
		//
		ret,
		unreachable,
		deopt,
	};

	// Conservative semantic effects. update() may add effects as operands become
	// more precise, but must never remove a previously published effect.
	//
	// - effect_read: observes mutable VM or heap state. DCE uses any non-empty
	//   effect set to retain otherwise-unused generic instructions.
	// - effect_write: mutates VM or heap state. Ownership treats it as a lifetime
	//   barrier because the write may replace a reference.
	// - effect_alloc: may allocate or trigger allocation bookkeeping; DCE must not
	//   discard it merely because its result is unused.
	// - effect_may_throw: may transfer control to an exception handler. Constant
	//   folding and ownership use it as an observable/control-flow barrier.
	// - effect_may_call_user: may invoke language code or metamethods. It implies
	//   effect_may_throw and effect_release_may_destroy; constant folding and
	//   ownership rely on that conservative closure.
	// - effect_release_may_destroy: may decrement the last reference and run
	//   destruction recursively. Ownership uses it as a lifetime barrier and DCE
	//   treats it as observable.
	//
	// opt_const consumes the dynamic-dispatch bits, opt_dce consumes the complete
	// set, and opt_rc consumes write/throw/user-call/destruction barriers.
	enum instruction_effect : uint32_t {
		effect_none                = 0,
		effect_read                = 1u << 0,
		effect_write               = 1u << 1,
		effect_alloc               = 1u << 2,
		effect_may_throw           = 1u << 3,
		effect_may_call_user       = 1u << 4,
		effect_release_may_destroy = 1u << 5,
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
		uint32_t effects         = effect_none;

		bool has_effect(instruction_effect effect) const { return (effects & effect) != 0; }

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
			parent = nullptr;
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
			T* copy       = new T(*(T*) this);
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
			effects |= effect_read | effect_alloc | effect_may_throw | effect_may_call_user | effect_release_may_destroy;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[0]->is<constant>());
			auto* c = operands[0]->as<constant>();
			LI_ASSERT(c->is(type::vmopr));
			LI_ASSERT(c->vmopr == operation::ANEG || c->vmopr == operation::LNOT);
			if (c->vmopr == operation::LNOT) {
				vt = type::i1;
			} else {
				is_pure   = false;
				sideffect = true;
				vt        = type::any;
				type_try_settle(operands[1]->vt, true);
			}
		}
		bool rec_type_check(type x) override {
			if (operands[0]->as<constant>()->vmopr == operation::LNOT)
				return x == type::i1;
			if (x > type::f64)
				return false;
			return operands[1]->type_try_settle(x);
		}
	};
	// unk  binop(const op, unk lhs, unk rhs)
	struct binop final : insn_tag<binop, opcode::binop> {
		bool is_exact_integer_arithmetic() const {
			if ((vt != type::i32 && vt != type::i64) || operands[1]->vt != vt || operands[2]->vt != vt)
				return false;
			auto op = operands[0]->as<constant>()->vmopr;
			return op == operation::AADD || op == operation::ASUB || op == operation::AMUL;
		}
		void update() override {
			effects |= effect_read | effect_alloc | effect_may_throw | effect_may_call_user | effect_release_may_destroy;
			LI_ASSERT(operands.size() == 3);
			LI_ASSERT(operands[0]->is<constant>());
			auto* c = operands[0]->as<constant>();
			LI_ASSERT(c->is(type::vmopr));
			LI_ASSERT(operation::AADD <= c->vmopr && c->vmopr <= operation::APOW);
			is_pure   = false;
			sideffect = true;

			// Integer range lowering establishes the complete same-width
			// contract before refresh; do not infer it from an operand alone.
			if (is_exact_integer_arithmetic())
				return;

			vt = type::any;
			if (operands[1]->vt <= type::f64) {
				type_try_settle(operands[1]->vt, true);
			} else if (operands[2]->vt <= type::f64) {
				type_try_settle(operands[2]->vt, true);
			}
		}
		bool rec_type_check(type x) override {
			if (x > type::f64)
				return false;
			return operands[1]->type_try_settle(x) && operands[2]->type_try_settle(x);
		}
	};

	// i1  compare(const op, unk lhs, unk rhs)
	struct compare final : insn_tag<compare, opcode::compare> {
		void update() override {
			effects |= effect_read | effect_alloc | effect_may_throw | effect_may_call_user | effect_release_may_destroy;
			vt = type::i1;
			LI_ASSERT(operands.size() == 3);
			LI_ASSERT(operands[0]->is<constant>());
			auto* c = operands[0]->as<constant>();
			LI_ASSERT(c->is(type::vmopr));
			LI_ASSERT(operation::CEQ <= c->vmopr && c->vmopr <= operation::CLE);
			is_pure   = false;
			sideffect = true;
		}
	};
	// i1   test_type(unk, const vty)
	struct test_type final : insn_tag<test_type, opcode::test_type> {
		void update() override {
			vt       = type::i1;
			is_const = true;  // Type of boxed types cannot change so this stays valid.
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::vty));
		}
	};
	// arr  array_new(i32)
	struct array_new final : insn_tag<array_new, opcode::array_new> {
		void update() override {
			effects |= effect_alloc;
			is_pure = false;
			vt      = type::arr;
			owner   = ownership_kind::owned;
			LI_ASSERT(operands.size() == 1);
			LI_ASSERT(operands[0]->is(type::i32));
		}
	};
	// tbl  table_new(i32)
	struct table_new final : insn_tag<table_new, opcode::table_new> {
		void update() override {
			effects |= effect_alloc;
			is_pure = false;
			vt      = type::tbl;
			owner   = ownership_kind::owned;
			LI_ASSERT(operands.size() == 1);
			LI_ASSERT(operands[0]->is(type::i32));
		}
	};
	// unk   uval_get(vfn, i32)
	struct uval_get final : insn_tag<uval_get, opcode::uval_get> {
		void update() override {
			effects |= effect_read;
			vt = type::any;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[0]->is(type::fn));
			LI_ASSERT(operands[1]->is(type::i32));
		}
	};
	// none  safepoint()
	struct safepoint final : insn_tag<safepoint, opcode::safepoint> {
		void update() override {
			effects |= effect_read | effect_write | effect_may_throw;
			is_pure   = false;
			sideffect = true;
			vt        = type::none;
			LI_ASSERT(operands.empty());
		}
	};
	// i32 va_count()
	struct va_count final : insn_tag<va_count, opcode::va_count> {
		void update() override {
			effects |= effect_read;
			is_pure = false;
			vt      = type::i32;
			LI_ASSERT(operands.empty());
		}
	};
	// any va_get(any index)
	struct va_get final : insn_tag<va_get, opcode::va_get> {
		void update() override {
			effects |= effect_read | effect_may_throw;
			is_pure   = false;
			sideffect = true;
			vt        = type::any;
			owner     = ownership_kind::owned;
			LI_ASSERT(operands.size() == 1);
		}
	};
	// T|i1 extract(T result, i32 index)
	struct extract final : insn_tag<extract, opcode::extract> {
		void update() override {
			is_const = true;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::i32));
			auto index = operands[1]->as<constant>()->i32;
			LI_ASSERT(index == 0 || index == 1);
			vt    = index == 0 ? operands[0]->vt : type::i1;
			owner = index == 0 ? operands[0]->owner : ownership_kind::borrowed;
		}
		bool rec_type_check(type x) override { return operands[1]->as<constant>()->i32 == 0 && operands[0]->type_try_settle(x); }
	};
	// any uval_set(vfn, i32, unk)
	struct uval_set final : insn_tag<uval_set, opcode::uval_set> {
		void update() override {
			effects |= effect_read | effect_write | effect_alloc | effect_may_throw | effect_release_may_destroy;
			is_pure   = false;
			sideffect = true;
			vt        = type::any;
			LI_ASSERT(operands.size() == 3);
			LI_ASSERT(operands[0]->is(type::fn));
			LI_ASSERT(operands[1]->is(type::i32));
		}
	};
	// any field_get(i1 raw, any obj, any key)
	struct field_get final : insn_tag<field_get, opcode::field_get> {
		void update() override {
			effects |= effect_read | effect_alloc | effect_may_throw | effect_may_call_user | effect_release_may_destroy;
			is_pure   = false;
			sideffect = true;
			vt        = type::any;
			LI_ASSERT(operands.size() == 3);
			LI_ASSERT(operands[0]->is<constant>() && operands[0]->is(type::i1));
			owner = ownership_kind::borrowed;
		}
	};
	// any field_set(i1 raw, any obj, any key, any val)
	struct field_set final : insn_tag<field_set, opcode::field_set> {
		void update() override {
			effects |= effect_read | effect_write | effect_alloc | effect_may_throw | effect_may_call_user | effect_release_may_destroy;
			is_pure   = false;
			sideffect = true;
			vt        = type::any;
			LI_ASSERT(operands.size() == 4);
		}
	};
	// any struct_array_get(tarr array, any index, vcl element_class)
	// Marker for a strict struct-array element read. It retains ordinary get
	// semantics unless opt_type proves that every materialized use is a field read.
	struct struct_array_get final : insn_tag<struct_array_get, opcode::struct_array_get> {
		void update() override {
			effects |= effect_read | effect_alloc | effect_may_throw | effect_release_may_destroy;
			is_pure   = false;
			sideffect = true;
			vt        = type::any;
			owner     = ownership_kind::borrowed;
			LI_ASSERT(operands.size() == 3);
			LI_ASSERT(operands[0]->is(type::tarr));
			LI_ASSERT(operands[2]->is<constant>() && operands[2]->is(type::vcl));
		}
	};
	// i1 struct_array_class_test(tarr array, vcl element_class)
	struct struct_array_class_test final : insn_tag<struct_array_class_test, opcode::struct_array_class_test> {
		void update() override {
			is_const = true;
			vt       = type::i1;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[0]->is(type::tarr));
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::vcl));
		}
	};
	// i1 struct_array_bounds_test(tarr array, int index)
	struct struct_array_bounds_test final : insn_tag<struct_array_bounds_test, opcode::struct_array_bounds_test> {
		void update() override {
			effects |= effect_read;
			is_pure = false;
			vt      = type::i1;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[0]->is(type::tarr));
			LI_ASSERT(operands[1]->is(type::i32));
		}
	};
	// T struct_array_field_load(tarr array, int index, i32 stride, i32 offset, dty T)
	struct struct_array_field_load final : insn_tag<struct_array_field_load, opcode::struct_array_field_load> {
		void update() override {
			effects |= effect_read;
			is_pure = false;
			LI_ASSERT(operands.size() == 5);
			LI_ASSERT(operands[0]->is(type::tarr));
			LI_ASSERT(operands[1]->is(type::i32));
			LI_ASSERT(operands[2]->is<constant>() && operands[2]->is(type::i32));
			LI_ASSERT(operands[3]->is<constant>() && operands[3]->is(type::i32));
			LI_ASSERT(operands[4]->is<constant>() && operands[4]->is(type::dty));
			auto storage = operands[4]->as<constant>()->dty;
			vt           = storage == type::f32 ? type::f64 : storage == type::i8 || storage == type::i16 ? type::i32 : storage;
		}
	};
	// any class_new(any class)
	struct class_new final : insn_tag<class_new, opcode::class_new> {
		void update() override {
			effects |= effect_read | effect_alloc | effect_may_throw;
			is_pure   = false;
			sideffect = true;
			vt        = type::any;
			owner     = ownership_kind::owned;
			LI_ASSERT(operands.size() == 1);
		}
	};
	// i1 class_is(any value, any class)
	struct class_is final : insn_tag<class_is, opcode::class_is> {
		void update() override {
			effects |= effect_read;
			vt = type::i1;
			LI_ASSERT(operands.size() == 2);
		}
	};
	// any iter_next(any target, i32 state_slot)
	struct iter_next final : insn_tag<iter_next, opcode::iter_next> {
		void update() override {
			effects |= effect_read | effect_write | effect_alloc | effect_may_throw | effect_may_call_user | effect_release_may_destroy;
			is_pure   = false;
			sideffect = true;
			vt        = type::any;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::i32));
		}
	};
	// T      assume_cast(unk, const dty T)
	struct assume_cast final : insn_tag<assume_cast, opcode::assume_cast> {
		void update() override {
			is_const = true;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::dty));
			vt    = operands[1]->as<constant>()->dty;
			owner = operands[0]->owner;
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
			effects |= effect_read;
			sideffect = true;
			vt        = type::none;
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
	// any osr_load(i32 slot)
	// Adopts the interpreter frame's owning local slot on an OSR entry edge: the
	// result is owned and the slot is left nil, so ownership moves exactly once.
	struct osr_load final : insn_tag<osr_load, opcode::osr_load> {
		void update() override {
			effects |= effect_read | effect_write;
			is_pure   = false;
			sideffect = true;
			vt        = type::any;
			owner     = ownership_kind::owned;
			LI_ASSERT(operands.size() == 1);
			LI_ASSERT(operands[0]->is<constant>() && operands[0]->is(type::i32));
		}
	};
	// none deopt(i32 bytecode_ip, (i32 slot, any value)...)
	// Every value operand is consumed: ownership moves into the frame slot.
	struct deopt final : insn_tag<deopt, opcode::deopt> {
		uint32_t exception_handler_pc = no_interpreter_handler;
		uint32_t cleanup_handler_pc   = no_interpreter_handler;

		void update() override {
			effects |= effect_read | effect_write | effect_may_throw;
			is_pure   = false;
			sideffect = true;
			vt        = type::none;
			LI_ASSERT(!operands.empty() && (operands.size() & 1));
			LI_ASSERT(operands[0]->is<constant>() && operands[0]->is(type::i32));
			for (size_t n = 1; n < operands.size(); n += 2)
				LI_ASSERT(operands[n]->is<constant>() && operands[n]->is(type::i32));
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
			vt      = type::none;
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
	// unk   load_local(i32)
	struct load_local final : insn_tag<load_local, opcode::load_local> {
		void update() override {
			effects |= effect_read;
			is_pure = true;
			vt      = type::any;
			LI_ASSERT(operands.size() == 1);
			LI_ASSERT(operands[0]->is<constant>() && operands[0]->is(type::i32));
		}
	};
	// none store_local(i32, unk)
	struct store_local final : insn_tag<store_local, opcode::store_local> {
		void update() override {
			effects |= effect_write | effect_release_may_destroy;
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
			is_const = true;
			LI_ASSERT(operands.size() == 1);
			vt    = operands[0]->vt;
			owner = operands[0]->owner;
		}
		bool rec_type_check(type x) override { return operands[0]->type_try_settle(x); }
	};
	// unk  erase_type(T x)
	struct erase_type final : insn_tag<erase_type, opcode::erase_type> {
		void update() override {
			is_const = true;
			LI_ASSERT(operands.size() == 1);
			vt    = type::any;
			owner = operands[0]->owner;
		}
	};
	// none keep_alive(unk value)
	struct keep_alive final : insn_tag<keep_alive, opcode::keep_alive> {
		void update() override {
			effects |= effect_read;
			is_pure   = false;
			sideffect = true;
			vt        = type::none;
			LI_ASSERT(operands.size() == 1);
		}
	};
	// T retain(T value)
	struct retain final : insn_tag<retain, opcode::retain> {
		void update() override {
			effects |= effect_read;
			is_pure   = false;
			sideffect = true;
			LI_ASSERT(operands.size() == 1);
			vt    = operands[0]->vt;
			owner = ownership_kind::owned;
		}
		bool rec_type_check(type x) override { return operands[0]->type_try_settle(x); }
	};
	// none release(any value)
	struct release final : insn_tag<release, opcode::release> {
		void update() override {
			effects |= effect_read | effect_write | effect_release_may_destroy;
			is_pure   = false;
			sideffect = true;
			vt        = type::none;
			LI_ASSERT(operands.size() == 1);
		}
	};
	// T ccall(nfni target, i32 overloadid, unk... args)
	struct ccall final : insn_tag<ccall, opcode::ccall> {
		void update() override {
			effects |= effect_read | effect_write | effect_alloc | effect_may_throw | effect_may_call_user | effect_release_may_destroy;
			LI_ASSERT(operands.size() >= 2);
			LI_ASSERT(operands[0]->is<constant>() && operands[0]->is(type::nfni));
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::i32));
			auto* nf  = operands[0]->as<constant>()->nfni;
			auto  ovl = operands[1]->as<constant>()->i32;
			vt        = nf->overloads[ovl].ret;
			owner     = ownership_kind::owned;
			is_pure   = nf->attr & func_attr_pure;
			is_const  = nf->attr & func_attr_const;
			sideffect = nf->attr & func_attr_sideeffect;
		}
	};
	// any set_exception(unk)
	struct set_exception final : insn_tag<set_exception, opcode::set_exception> {
		void update() override {
			effects |= effect_read | effect_write | effect_alloc | effect_may_throw | effect_release_may_destroy;
			is_pure   = false;
			sideffect = true;
			vt        = type::any;
			LI_ASSERT(operands.size() == 1);
		}
	};
	// unk  get_exception()
	struct get_exception final : insn_tag<get_exception, opcode::get_exception> {
		void update() override {
			effects |= effect_read;
			is_pure = false;
			vt      = type::any;
			LI_ASSERT(operands.size() == 0);
		}
	};
	// unk vcall(unk target, unk self, unk... args)
	struct vcall final : insn_tag<vcall, opcode::vcall> {
		void update() override {
			effects |= effect_read | effect_write | effect_alloc | effect_may_throw | effect_may_call_user | effect_release_may_destroy;
			is_pure   = false;
			sideffect = true;
			vt        = type::any;
			owner     = ownership_kind::owned;
			LI_ASSERT(operands.size() >= 2);
		}
	};
};