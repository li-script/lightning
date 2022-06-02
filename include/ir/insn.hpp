#pragma once
#include <vector>
#include <string>
#include <ir/value.hpp>

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

		// Used by initial codegen.
		//
		load_local,
		store_local,

		// Used by intermediate code.
		//
		land,
		lor,
		lnot,
		null_coalesce,
		check_type,
		check_trait,
		trait_get,
		trait_set,
		array_new,
		table_new,
		dup,
		vlen,
		vin,
		vjoin,
		uval_get,
		uval_getx,
		uval_set,
		uval_setx,
		field_get,
		field_get_raw,
		field_set,
		field_set_raw,

		// Used by all levels of codegen.
		//
		select,
		unop,
		binop,
		compare,
		phi,

		// Block terminators.
		//
		jmp,
		jcc,

		// Procedure terminators.
		//
		ret,
		thrw,
	};

	// Instruction type.
	//
	struct insn : value_tag<insn> {
		// Handles constants.
		//
		template<typename Tv>
		static value_ref value_launder(Tv&& v) {
			if constexpr (!std::is_convertible_v<Tv, value_ref>) {
				return std::make_shared<constant>(std::forward<Tv>(v));
			} else {
				return value_ref(v);
			}
		}

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

		// Instruction side effects.
		//
		uint8_t may_gc : 1              = 0;
		uint8_t may_throw : 1           = 0;
		uint8_t is_volatile : 1         = 0;

		// Operands.
		//
		std::vector<value_ref> operands;

		// Implement printer.
		//
		std::string to_string(bool expand = false) const override;

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
	//   Missing: push,load,reset,call
	//
	//	// Call to C.
	// // Unpacking / Repacking.
	//	ccall,  //      T    ccall(const ptr target, ...)
	//
	// unk  unop(const op, unk rhs)
	struct unop final : insn_tag<opcode::unop> {
		void update() override {
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[0]->is<constant>());
			auto* c = operands[0]->get<constant>();
			LI_ASSERT(c->is(type::vmopr));
			LI_ASSERT(c->vmopr == operation::ANEG);
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
	// unk  land(unk lhs, unk rhs)
	struct land final : insn_tag<opcode::land> {
		void update() override { LI_ASSERT(operands.size() == 2); }
	};
	// unk  null_coalesce(unk lhs, unk rhs)
	struct null_coalesce final : insn_tag<opcode::null_coalesce> {
		void update() override { LI_ASSERT(operands.size() == 2); }
	};
	// unk  lor(unk lhs, unk rhs)
	struct lor final : insn_tag<opcode::lor> {
		void update() override { LI_ASSERT(operands.size() == 2); }
	};
	// i1   lnot(unk lhs)
	struct lnot final : insn_tag<opcode::lnot> {
		void update() override {
			vt = type::i1;
			LI_ASSERT(operands.size() == 1);
		}
	};
	// unk  select(i1 cc, unk t, unk f)
	struct select final : insn_tag<opcode::select> {
		void update() override {
			LI_ASSERT(operands.size() == 3);
			LI_ASSERT(operands[0]->is(type::i1));
		}
	};
	// i1   check_type(unk, const vmtype)
	struct check_type final : insn_tag<opcode::check_type> {
		void update() override {
			vt = type::i1;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::vmtype));
		}
	};
	// i1   check_trait(unk, const vmtrait)
	struct check_trait final : insn_tag<opcode::check_trait> {
		void update() override {
			vt = type::i1;
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
			vt = type::none;
			LI_ASSERT(operands.size() == 3);
			LI_ASSERT(operands[1]->is<constant>() && operands[1]->is(type::vmtrait));
		}
	};
	// arr  array_new(i32)
	struct array_new final : insn_tag<opcode::array_new> {
		void update() override {
			vt = type::arr;
			LI_ASSERT(operands.size() == 1);
			LI_ASSERT(operands[0]->is(type::i32));
		}
	};
	// tbl  table_new(i32)
	struct table_new final : insn_tag<opcode::table_new> {
		void update() override {
			vt = type::tbl;
			LI_ASSERT(operands.size() == 1);
			LI_ASSERT(operands[0]->is(type::i32));
		}
	};
	// T     dup(T)
	struct dup final : insn_tag<opcode::dup> {
		void update() override {
			LI_ASSERT(operands.size() == 1);
			vt = operands[0]->vt;
		}
	};
	// unk   uval_get(i32)
	struct uval_get final : insn_tag<opcode::uval_get> {
		void update() override {
			LI_ASSERT(operands.size() == 1);
			LI_ASSERT(operands[0]->is(type::i32));
		}
	};
	// none   uval_set(i32, unk)
	struct uval_set final : insn_tag<opcode::uval_set> {
		void update() override {
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[0]->is(type::i32));
		}
	};
	// unk   uval_getx(vfn, i32)
	struct uval_getx final : insn_tag<opcode::uval_getx> {
		void update() override {
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[0]->is(type::vfn));
			LI_ASSERT(operands[1]->is(type::i32));
		}
	};
	// none   uval_setx(vfn, i32, unk)
	struct uval_setx final : insn_tag<opcode::uval_setx> {
		void update() override {
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
			vt = type::none;
			LI_ASSERT(operands.size() == 3);
		}
	};
	// none   ret(unk val)
	struct ret final : insn_tag<opcode::ret> {
		void update() override {
			vt = type::none;
			LI_ASSERT(operands.size() == 1);
		}
	};
	// none   thrw(unk val)
	struct thrw final : insn_tag<opcode::thrw> {
		void update() override {
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
			if (size_t n = operands.size()) {
				for (size_t i = 0; i != n; i++) {
					// TODO: Validate CFG.
					LI_ASSERT(operands[i]->is<insn>());
				}
				vt = type::unk;  // calc common type.
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
			vt = type::unk;
			LI_ASSERT(operands.size() == 1);
			LI_ASSERT(operands[0]->is<constant>() && operands[0]->is(type::i32));
		}
	};
	// none store_local(const i32, unk)
	struct store_local final : insn_tag<opcode::store_local> {
		void update() override {
			vt = type::none;
			LI_ASSERT(operands.size() == 2);
			LI_ASSERT(operands[0]->is<constant>() && operands[0]->is(type::i32));
		}
	};
};