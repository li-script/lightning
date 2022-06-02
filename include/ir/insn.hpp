#pragma once
#include <vector>
#include <string>
#include <ir/value.hpp>
#include <ir/arch.hpp>

/*
_(get) _(set)
_(call) _(str) _(gc)
TODO: Consider __(seal) __(freeze) __(hide)
*/

namespace li::ir {
	// Instruction opcodes.
	//
	enum class opcode : uint8_t {
		invalid,

		// Used by intermediate code.
		//
		load_local,
		store_local,

		// Complex types.
		//
		dup,
		vlen,
		vin,
		vjoin,
		trait_get,
		trait_set,
		array_new,
		table_new,
		field_get,
		field_get_raw,
		field_set,
		field_set_raw,

		// Arithmetic operators.
		//
		unop,
		binop,

		// Upvalue.
		//
		uval_get,
		uval_set,

		// Casts.
		//
		try_cast,
		assume_cast,
		coerce_cast,

		// Conditionals.
		//
		test_type,
		test_trait,
		compare,
		select,
		phi,

		// Block terminators.
		//
		jmp,
		jcc,

		// Call types.
		//
		ccall,
		vcall,

		// Procedure terminators.
		//
		ret,
		thrw,

		// Hints.
		//
		mark_used
	};

	// Instruction type.
	//
	struct insn : value_tag<insn> {

		// Parent block.
		//
		basic_block* parent = nullptr;

		// Numbered name of the instruction value.
		//
		uint32_t name = 0;

		// Opcode.
		//
		opcode op = opcode::invalid;

		// Source/Line information.
		//
		uint32_t source_bc   = UINT32_MAX;

		// Traits.
		//
		uint32_t alias : 1       = 0;  // Result always is one of the register operands.
		uint32_t is_pure : 1     = 1;  // Always returns the same value given the same arguments (unless there was an instruction with sideeffects).
		uint32_t is_const : 1    = 0;  // On top of being pure, also doesn't break the constraints on sideffect.
		uint32_t sideffect : 1   = 0;  // Has side effects and should not be discarded if not used.
		uint32_t is_volatile : 1 = 0;  // Same as side-effect, but user specified and cannot be ignored by instruction-specific optimizers.
		uint32_t spill_gc : 1    = 0;  // Spills all locals that are GC'd.
		uint32_t spill_vol : 1   = 0;  // Spills volatile registers.

		// Spilled registers.
		//
		std::vector<arch::reg> spilled_regs = {};

		// Forced register ids.
		//
		arch::reg force_result_reg = 0;
		std::vector<arch::reg> force_operand_regs = {};

		// Operands.
		//
		std::vector<value_ref> operands;

		// Implement printer.
		//
		std::string to_string(bool expand = false) const override;

		// Copies debug info to another instance.
		//
		void copy_debug_info(insn* o) { o->source_bc = source_bc; }

		// Basic traits.
		//
		bool is_terminator() const { return op >= opcode::jmp; }
		bool is_proc_terminator() const { return op >= opcode::ret; }

		// Updates the instruction details such as return type/side effects.
		// Throws in debug mode if it's in an invalid state.
		//
		virtual void update() {}
	};
	using insn_ref = std::shared_ptr<insn>;

	// Instruction tag.
	//
	template<opcode O>
	struct insn_tag : insn {
		static constexpr opcode Opcode = O;
	};
	
	// Individual instructions.
	//   Missing: push,load,reset
	// // Unpacking / Repacking.
	//
	// unk  unop(const op, unk rhs)
	struct unop final : insn_tag<opcode::unop> {
		void update() override {
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[0]->is<constant>());
			auto* c = operands[0]->get<constant>();
			LI_ASSERT(c->is(type::vmopr));
			LI_ASSERT(c->vmopr == operation::ANEG || c->vmopr == operation::LNOT);

			if (operands[1]->vt <= type::f64) {
				vt = operands[1]->vt;
			}
		}
	};
	// unk  binop(const op, unk lhs, unk rhs)
	struct binop final : insn_tag<opcode::binop> {
		void update() override {
			LI_ASSERT(operands.size() == 3);
			LI_ASSERT(operands[0]->is<constant>());
			auto* c = operands[0]->get<constant>();
			LI_ASSERT(c->is(type::vmopr));
			LI_ASSERT(operation::AADD <= c->vmopr && c->vmopr <= operation::APOW);

			if (operands[1]->vt <= type::f64 && operands[2]->vt <= type::f64) {
				vt = std::max(operands[1]->vt, operands[2]->vt);
			}
		}
	};

	// i1  compare(const op, unk lhs, unk rhs)
	struct compare final : insn_tag<opcode::compare> {
		void update() override {
			vt = type::i1;
			LI_ASSERT(operands.size() == 3);
			LI_ASSERT(operands[0]->is<constant>());
			auto* c = operands[0]->get<constant>();
			LI_ASSERT(c->is(type::vmopr));
			LI_ASSERT(operation::CEQ <= c->vmopr && c->vmopr <= operation::CGE);
		}
	};
	// unk  select(i1 cc, unk t, unk f)
	struct select final : insn_tag<opcode::select> {
		void update() override {
			alias = true;
			is_const = true;
			LI_ASSERT(operands.size() == 3);
			LI_ASSERT(operands[0]->is(type::i1));
			if (operands[0]->vt == operands[1]->vt) {
				vt = operands[0]->vt;
			}
		}
	};
	// i1   test_type(unk, const vmtype)
	struct test_type final : insn_tag<opcode::test_type> {
		void update() override {
			vt = type::i1;
			is_const = true; // Type of boxed types cannot change so this stays valid.
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::vmtype));
		}
	};
	// i1   test_trait(unk, const vmtrait)
	struct test_trait final : insn_tag<opcode::test_trait> {
		void update() override {
			vt      = type::i1;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::vmtrait));
		}
	};
	// unk   trait_get(unk, const vmtrait)
	struct trait_get final : insn_tag<opcode::trait_get> {
		void update() override {
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::vmtrait));
		}
	};
	// none  trait_set(unk, const vmtrait, unk)
	struct trait_set final : insn_tag<opcode::trait_set> {
		void update() override {
			is_pure   = false;
			sideffect = true;
			vt = type::none;
			LI_ASSERT(operands.size() == 3);
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::vmtrait));
		}
	};
	// arr  array_new(i32)
	struct array_new final : insn_tag<opcode::array_new> {
		void update() override {
			is_pure = false;
			vt = type::arr;
			LI_ASSERT(operands.size() == 1);
			LI_ASSERT(operands[0]->is(type::i32));
		}
	};
	// tbl  table_new(i32)
	struct table_new final : insn_tag<opcode::table_new> {
		void update() override {
			is_pure = false;
			vt = type::tbl;
			LI_ASSERT(operands.size() == 1);
			LI_ASSERT(operands[0]->is(type::i32));
		}
	};
	// T     dup(T)
	struct dup final : insn_tag<opcode::dup> {
		void update() override {
			is_pure = false;
			LI_ASSERT(operands.size() == 1);
			vt = operands[0]->vt;
		}
	};
	// unk   uval_get(vfn, i32)
	struct uval_get final : insn_tag<opcode::uval_get> {
		void update() override {
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[0]->is(type::vfn));
			LI_ASSERT(operands[1]->is(type::i32));
		}
	};
	// none   uval_set(vfn, i32, unk)
	struct uval_set final : insn_tag<opcode::uval_set> {
		void update() override {
			is_pure   = false;
			sideffect = true;
			LI_ASSERT(operands.size() == 3);
			LI_ASSERT(operands[0]->is(type::vfn));
			LI_ASSERT(operands[1]->is(type::i32));
		}
	};
	// unk    field_get(unk obj, unk key)
	struct field_get final : insn_tag<opcode::field_get> {
		void update() override { LI_ASSERT(operands.size() == 2); }
	};
	// none   field_set(unk obj, unk key, unk val)
	struct field_set final : insn_tag<opcode::field_set> {
		void update() override {
			is_pure   = false;
			sideffect = true;
			vt = type::none;
			LI_ASSERT(operands.size() == 3);
		}
	};
	// unk    field_get_raw(unk obj, unk key)
	struct field_get_raw final : insn_tag<opcode::field_get_raw> {
		void update() override { LI_ASSERT(operands.size() == 2); }
	};
	// none   field_set_raw(unk obj, unk key, unk val)
	struct field_set_raw final : insn_tag<opcode::field_set_raw> {
		void update() override {
			is_pure   = false;
			sideffect = true;
			vt = type::none;
			LI_ASSERT(operands.size() == 3);
		}
	};
	// T      try_cast(unk, const irtype T)
	struct try_cast final : insn_tag<opcode::try_cast> {
		void update() override {
			alias    = true;
			is_const = true;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::irtype));
			vt        = operands[1]->get<constant>()->irtype;
		}
	};
	// T      assume_cast(unk, const irtype T)
	struct assume_cast final : insn_tag<opcode::assume_cast> {
		void update() override {
			alias    = true;
			is_const = true;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::irtype));
			vt        = operands[1]->get<constant>()->irtype;
		}
	};
	// T      coerce_cast(unk, const irtype T)
	struct coerce_cast final : insn_tag<opcode::coerce_cast> {
		void update() override {
			is_const = true;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::irtype));
			vt = operands[1]->get<constant>()->irtype;
			alias = operands[0]->vt == vt;
		}
	};
	// none   ret(unk val)
	struct ret final : insn_tag<opcode::ret> {
		void update() override {
			sideffect = true;
			vt = type::none;
			LI_ASSERT(operands.size() == 1);
		}
	};
	// none   thrw(unk val)
	struct thrw final : insn_tag<opcode::thrw> {
		void update() override {
			sideffect = true;
			vt = type::none;
			LI_ASSERT(operands.size() == 1);
		}
	};
	// none   jmp(const bb)
	struct jmp final : insn_tag<opcode::jmp> {
		void update() override {
			vt = type::none;
			LI_ASSERT(operands.size() == 1);
			LI_ASSERT(operands[0]->is<constant>() && operands[0]->is(type::bb));
		}
	};
	// none   jcc(i1 c, const bb t, const bb f)
	struct jcc final : insn_tag<opcode::jcc> {
		void update() override {
			is_pure = false;
			vt = type::none;
			LI_ASSERT(operands.size() == 3);
			LI_ASSERT(operands[0]->is(type::i1));
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::bb));
			LI_ASSERT(operands[2]->is<constant>() && operands[2]->is(type::bb));
		}
	};
	// unk   phi(unk...)
	struct phi final : insn_tag<opcode::phi> {
		void update() override {
			alias    = true;
			is_const = true;

			if (size_t n = operands.size()) {
				vt = operands[0]->vt;
				for (size_t i = 0; i != n; i++) {
					if (vt != operands[i]->vt) {
						vt = type::unk;
					}
				}
			} else {
				vt = type::none;
			}
		}
	};
	// unk  vlen(unk obj)
	struct vlen final : insn_tag<opcode::vlen> {
		void update() override { LI_ASSERT(operands.size() == 1); }
	};
	// i1   vin(unk obj, unk collection)
	struct vin final : insn_tag<opcode::vin> {
		void update() override {
			vt = type::i1;
			LI_ASSERT(operands.size() == 2);
		}
	};
	// unk  vjoin(unk a, unk b)
	struct vjoin final : insn_tag<opcode::vjoin> {
		void update() override {
			LI_ASSERT(operands.size() == 2);
			if (operands[0]->is(type::unk)) {
				vt = operands[1]->vt;
			} else if (operands[1]->is(type::unk)) {
				vt = operands[0]->vt;
			} else {
				vt = operands[0]->vt;
				LI_ASSERT(operands[0]->vt == operands[1]->vt);
			}
			LI_ASSERT(vt == type::unk || vt == type::tbl || vt == type::arr);
		}
	};
	// unk  load_local(const i32)
	struct load_local final : insn_tag<opcode::load_local> {
		void update() override {
			is_pure = true;
			vt = type::unk;
			LI_ASSERT(operands.size() == 1);
			LI_ASSERT(operands[0]->is<constant>() && operands[0]->is(type::i32));
		}
	};
	// none store_local(const i32, unk)
	struct store_local final : insn_tag<opcode::store_local> {
		void update() override {
			is_pure   = false;
			sideffect = true;
			vt        = type::none;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[0]->is<constant>() && operands[0]->is(type::i32));
		}
	};
	// T ccall(irtype rettype, ptr target, ...)
	struct ccall final : insn_tag<opcode::ccall> {
		void update() override {
			is_pure   = false;
			sideffect = true;
			spill_vol = true;
			LI_ASSERT(operands.size() >= 2);
			LI_ASSERT(operands[0]->is<constant>() && operands[0]->is(type::irtype));
			LI_ASSERT(operands[1]->is(type::ptr));
			vt = operands[0]->get<constant>()->irtype;
			if (vt == type::f32 || vt == type::f64) {
				force_result_reg = arch::fp_retval;
			}

			force_operand_regs.clear();
			force_operand_regs.reserve(operands.size());
			size_t num_fp = 0;
			size_t num_gp = 0;
			for (auto& op : operands) {
				bool is_fp = op->vt == type::f32 || op->vt == type::f64;
				auto reg = arch::map_argument(num_gp, num_fp, is_fp);
				if (!reg)
					break;
				force_operand_regs.emplace_back(reg);
			}
		}
	};
	// any vcall(const i32 fixedargs)
	struct vcall final : insn_tag<opcode::vcall> {
		void update() override {
			is_pure   = false;
			sideffect = true;
			spill_gc  = true;
			spill_vol = true;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[0]->is<constant>() && operands[0]->is(type::i32));
		}
	};
	// none mark_used(unk...)
	struct mark_used final : insn_tag<opcode::mark_used> {
		void update() override {
			is_pure   = false;
			sideffect = true;
		}
	};
};