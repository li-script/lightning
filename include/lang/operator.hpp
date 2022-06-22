#pragma once
#include <vm/types.hpp>
#include <vm/bc.hpp>
#include <lang/lexer.hpp>
#include <tuple>

namespace li {
	struct vm;

	// Applies the unary/binary operator the values given. On failure (e.g. type mismatch),
	// returns the exception as the result.
	//
	LI_INLINE any apply_unary(vm* L, any a, bc::opcode op);
	LI_INLINE any apply_binary(vm* L, any a, any b, bc::opcode op);

	// List of operators with their traits.
	//
	struct operator_traits {
		// Lexer token and the emitted bytecode.
		//
		lex::token token;
		std::optional<lex::token> compound_token = {};
		bc::opcode opcode;

		// Precedence.
		//
		uint8_t prio_left;
		uint8_t prio_right;
	};
	static constexpr operator_traits unary_operators[] = {
		 {lex::token_lnot, {}, bc::LNOT, 3, 2},
		 {lex::token_sub, {}, bc::ANEG, 3, 2},
		 {lex::token_add, {}, bc::NOP, 0, 0},
	};
	static constexpr operator_traits binary_operators[] = {
		 {lex::token_add, lex::token_cadd, bc::AADD, 6, 6},
		 {lex::token_sub, lex::token_csub, bc::ASUB, 6, 6},
		 {lex::token_mul, lex::token_cmul, bc::AMUL, 5, 5},
		 {lex::token_div, lex::token_cdiv, bc::ADIV, 5, 5},
		 {lex::token_mod, lex::token_cmod, bc::AMOD, 5, 5},
		 {lex::token_pow, lex::token_cpow, bc::APOW, 5, 5},
		 {lex::token_nullc, lex::token_cnullc, bc::NCS, 13, 13},
		 {lex::token_land, {}, bc::LAND, 14, 14},
		 {lex::token_lor, {}, bc::LOR, 15, 15},
		 {lex::token_eq, {}, bc::CEQ, 10, 10},
		 {lex::token_ne, {}, bc::CNE, 10, 10},
		 {lex::token_lt, {}, bc::CLT, 9, 9},
		 {lex::token_gt, {}, bc::CGT, 9, 9},
		 {lex::token_le, {}, bc::CLE, 9, 9},
		 {lex::token_ge, {}, bc::CGE, 9, 9},
	};
};