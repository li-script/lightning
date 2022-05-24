#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <stdio.h>
#include <util/format.hpp>

namespace lightning::lexer {
	// Token enumerator:
	//  _    => keyword        | "let"
	//  __   => symbol mapped  | "--"
	//  ___  => single char    | '+'
	//  ____ => literal        | <string>
	//
#define LIGHTNING_ENUM_TOKENS(_, __, ___, ____)                             \
		/* Binary operators */																 \
		___(band, '&') ___(bor, '|') ___(bxor, '^') ___(bnot, '~') 				 \
		__(bshr, >>) __(bshl, <<)															 \
		/* Logical operators */																 \
		__(and, &&) __(or, ||) ___(not, '!') __(eq, ==) __(ne, !=)				 \
		___(le, '<') ___(gt, '>') __(le_eq, <=) __(gt_eq, >=)						 \
		/* Arithmetic operators */															 \
		___(add, '+') ___(sub, '-') ___(mul, '*') ___(div, '/') 					 \
		___(mod, '%')																			 \
		/* Ternary operator */																 \
		___(tif, '?') ___(telse, ':')														 \
		/* Compound operators */															 \
		__(cinc, ++) __(cdec, --) __(cadd, +=) __(csub, -=)						 \
		__(cmul, *=) __(cdiv, /=) __(cmod, %=) __(cband, &=)						 \
		__(cbor, |=) __(cbxor, ^=) __(cbshr, >>=) __(cbshl, <<=)					 \
		/* Language operators */															 \
		__(dots, ...) __(index_if, ?.)                                   		 \
		/* Literal tokens */																	 \
		____(eof, <eof>) ____(number, <number>) ____(integer, <integer>)      \
		____(name, <name>) ____(string, <string>) ____(error, <error>) 		 \
		/* Keywords */																			 \
		_(true) _(false)  _(let) _(const) _(if) _(else) _(switch) _(while)    \
		_(for) _(loop) _(case) _(default) _(break) _(continue) _(try)			 \
		_(catch) _(return) _(fn) _(in)	                   

	// Token identifiers.
	//
	enum token : uint8_t
	{
		#define TK_NOOP(...)
		#define TK_CHAR(name, chr) token_##name = chr,
		#define TK_NAME(name, sym) token_##name,
		#define TK_RET(name, sym) return token_##name;

		// Character tokens, end at 0x7F.
		LIGHTNING_ENUM_TOKENS(TK_NOOP, TK_NOOP, TK_CHAR, TK_NOOP)
		token_char_max = 0x7F,

		// Symbolic tokens.
		LIGHTNING_ENUM_TOKENS(TK_NOOP, TK_NAME, TK_NOOP, TK_NOOP)
		// Named tokens.
		LIGHTNING_ENUM_TOKENS(TK_NAME, TK_NOOP, TK_NOOP, TK_NOOP)
		// Literal tokens.
		LIGHTNING_ENUM_TOKENS(TK_NOOP, TK_NOOP, TK_NOOP, TK_NAME)
		
		token_lit_max_plus_one,
		token_lit_max =  token_lit_max_plus_one - 1,
		token_sym_min =  token_char_max + 1,
		token_name_min = []() { LIGHTNING_ENUM_TOKENS(TK_RET, TK_NOOP, TK_NOOP, TK_NOOP); }(),
		token_lit_min =  []() { LIGHTNING_ENUM_TOKENS(TK_NOOP, TK_NOOP, TK_NOOP, TK_RET); }(),
		token_sym_max =  token_name_min,
		token_name_max = token_lit_min,

		#undef TK_RET
		#undef TK_CHAR
		#undef TK_NAME
		#undef TK_NOOP
	};

	// Complex token to string conversion.
	//
	static constexpr std::string_view cx_token_to_str_map[] = {
		#define TK_NOOP(...)
		#define TK_SYM(name)       #name,
		#define TK_NAME(name, sym) #sym,

		// Symbolic tokens.
		LIGHTNING_ENUM_TOKENS(TK_NOOP, TK_NAME, TK_NOOP, TK_NOOP)
		// Named tokens.
		LIGHTNING_ENUM_TOKENS(TK_SYM, TK_NOOP, TK_NOOP, TK_NOOP)
		// Literal tokens.
		LIGHTNING_ENUM_TOKENS(TK_NOOP, TK_NOOP, TK_NOOP, TK_NAME)

		#undef TK_SYM
		#undef TK_NAME
		#undef TK_NOOP
	};
	static constexpr std::string_view cx_token_to_strv(uint8_t tk) {
		if (token_sym_min <= tk && tk <= token_lit_max) {
			return cx_token_to_str_map[uint8_t(tk) - token_sym_min];
		}
		return {};
	}

	// Handles escapes within a string.
	//
	std::string escape(std::string_view str);

	// Token value.
	//
	struct token_value {
		// Identifier.
		//
		token id = token_eof;

		// Value.
		//
		union {
			std::string_view str_val; // token_string, token_name, note: token_string is not escaped!
			double           num_val; // token_number
			int64_t          int_val; // token_integer
		};

		// String conversion.
		//
		std::string to_string() const {
			// Char token.
			if (id <= token_char_max) {
				std::string result;
				result += (char) id;
				return result;
			}
			// Named/Symbolic token.
			else if (id < token_lit_min) {
				return std::string(cx_token_to_strv(id));
			}
			// String literal.
			else if (id == token_string) {
				return util::fmt("\"%.*s\"", str_val.size(), str_val.data());
			}
			// Identifier.
			else if (id == token_name) {
				return util::fmt("<name: %.*s>", str_val.size(), str_val.data());
			} 
			// Numeric literal.
			else if (id == token_number) {
				return util::fmt("<num: %lf>", num_val);
			} else {
				return util::fmt("<int: %lld>", int_val);
			}
		}
		void print() const {
			// Char token.
			if (id <= token_char_max) {
				printf(LI_BRG "%c" LI_DEF, id);
			}
			// Named/Symbolic token.
			else if (id < token_lit_min) {
				auto token_str = cx_token_to_strv(id);
				printf(LI_PRP "%.*s" LI_DEF, token_str.size(), token_str.data());
			}
			// String literal.
			else if (id == token_string) {
				printf(LI_BLU "%.*s" LI_DEF, str_val.size(), str_val.data());
			}
			// Identifier.
			else if (id == token_name) {
				printf(LI_RED "%.*s" LI_DEF, str_val.size(), str_val.data());
			}
			// Numeric literal.
			else if (id == token_number) {
				printf(LI_CYN "%lf" LI_DEF, num_val);
			} else {
				printf(LI_BLU "%lld" LI_DEF, int_val);
			}
		}
	};

	// Lexer state.
	//
	struct state {
		// Input data and current location.
		//
		std::string input_buffer;
		std::string_view input = {};

		// Current line index.
		//
		size_t line = 1;

		// Current and lookahead token.
		//
		token_value tok_current                  = {};
		std::optional<token_value> tok_lookahead = {};

		// Last lexer error.
		//
		std::string last_error = {};

		// Initialized with a string buffer.
		//
		state(std::string data);

		// No copy.
		//
		state(const state&)            = delete;
		state& operator=(const state&) = delete;

		// Error helper.
		//
		template<typename... Tx>
		token_value error(const char* fmt, Tx... args) {
			last_error = util::fmt(fmt, args...);
			return token_value{.id = token_error};
		}

		// Gets the next/lookahead token.
		//
		token_value& next();
		token_value& lookahead();
	};
};