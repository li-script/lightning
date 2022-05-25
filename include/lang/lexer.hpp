#pragma once
#include <stdio.h>
#include <lang/types.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <util/format.hpp>
#include <vm/string.hpp>

namespace lightning::core {
	struct vm;
};

namespace lightning::lex {
	// Token enumerator:
	//  _    => keyword        | "let"
	//  __   => symbol mapped  | "--"
	//  ___  => single char    | '+'
	//  ____ => literal        | <string>
	//
#define LIGHTNING_ENUM_TOKENS(_, __, ___, ____)                                \
	/* Logical operators */                                                     \
		__(land, &&) __(lor, ||) ___(lnot, '!') __(eq, ==) __(ne, !=)			    \
		___(lt, '<') ___(gt, '>') __(le, <=) __(ge, >=)						          \
		/* Arithmetic operators */															    \
		___(add, '+') ___(sub, '-') ___(mul, '*') ___(div, '/') 					    \
		___(mod, '%') ___(pow, '^')															 \
		/* Ternary operator */																    \
		___(tif, '?') ___(telse, ':')														    \
		/* Compound operators */															    \
		__(cadd, +=) __(csub, -=) __(cmul, *=) __(cdiv, /=) __(cmod, %=)         \
		__(cpow, ^=) __(ccat, ..=)                                      			 \
		/* Language operators */															    \
		__(dots, ...) __(cat, ..)                                                \
		/* Literal tokens */																	    \
		____(eof, <eof>) ____(lnum, <number>) ____(name, <name>)                 \
		____(lstr, <string>) ____(error, <error>) 		                         \
		/* Keywords */																			    \
		_(true) _(false)  _(let) _(const) _(if) _(else) _(switch) _(while)       \
		_(for) _(loop) _(case) _(default) _(break) _(continue) _(try)			    \
		_(catch) _(throw) _(return) _(fn) _(in) _(is) _(bool) _(number) _(table) \
		_(array) _(userdata) _(function) _(thread)

	// Token identifiers.
	//
	enum token : uint8_t {
#define TK_NOOP(...)
#define TK_CHAR(name, chr) token_##name = chr,
#define TK_NAME(name, ...) token_##name,
#define TK_RET(name, ...)  return token_##name;

		// Character tokens, end at 0x7F.
		LIGHTNING_ENUM_TOKENS(TK_NOOP, TK_NOOP, TK_CHAR, TK_NOOP) token_char_max = 0x7F,

		// Symbolic tokens.
		LIGHTNING_ENUM_TOKENS(TK_NOOP, TK_NAME, TK_NOOP, TK_NOOP)
		// Named tokens.
		LIGHTNING_ENUM_TOKENS(TK_NAME, TK_NOOP, TK_NOOP, TK_NOOP)
		// Literal tokens.
		LIGHTNING_ENUM_TOKENS(TK_NOOP, TK_NOOP, TK_NOOP, TK_NAME)

		token_lit_max_plus_one,
		token_lit_max  = token_lit_max_plus_one - 1,
		token_sym_min  = token_char_max + 1,
		token_name_min = []() { LIGHTNING_ENUM_TOKENS(TK_RET, TK_NOOP, TK_NOOP, TK_NOOP); }(),
		token_lit_min = []() { LIGHTNING_ENUM_TOKENS(TK_NOOP, TK_NOOP, TK_NOOP, TK_RET); }(),
		token_sym_max  = token_name_min,
		token_name_max = token_lit_min,

#undef TK_RET
#undef TK_CHAR
#undef TK_NAME
#undef TK_NOOP
	};

	// Token traits.
	//
	LI_INLINE static constexpr bool is_token_literal(uint8_t t) { return token_lit_min <= t && token_lit_max; }
	LI_INLINE static constexpr bool is_token_symbolic(uint8_t t) { return token_sym_min <= t && token_sym_max; }
	LI_INLINE static constexpr bool is_token_keyword(uint8_t t) { return token_name_min <= t && token_name_max; }
	LI_INLINE static constexpr bool is_token_character(uint8_t t) { return t <= token_char_max; }
	LI_INLINE static constexpr bool is_token_complex(uint8_t t) { return token_sym_min <= t && t <= token_lit_max; }

	// Complex token to string conversion.
	//
	static constexpr std::string_view cx_token_to_str_map[] = {
#define TK_NOOP(...)
#define TK_SYM(name, ...)  #name,
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
		if (is_token_complex(tk)) {
			return cx_token_to_str_map[uint8_t(tk) - token_sym_min];
		}
		return {};
	}

	// Token value.
	//
	struct token_value {
		// Identifier.
		//
		token id = token_eof;

		// Value.
		//
		union {
			core::string* str_val;  // token_lstr, token_name
			core::number  num_val;  // token_lnum
		};

		// Equality comparable with token id.
		//
		inline constexpr bool operator==(uint8_t t) const { return id == t; }
		inline constexpr bool operator!=(uint8_t t) const { return id != t; }

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
			else if (id == token_lstr) {
				return util::fmt("\"%s\"", str_val->c_str());
			}
			// Identifier.
			else if (id == token_name) {
				return util::fmt("<name: %s>", str_val->c_str());
			}
			// Numeric literal.
			else {
				return util::fmt("<num: %lf>", num_val);
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
				printf(LI_PRP "%.*s" LI_DEF, (int) token_str.size(), token_str.data());
			}
			// String literal.
			else if (id == token_lstr) {
				printf(LI_BLU "\"%s\"" LI_DEF, str_val->c_str());
			}
			// Identifier.
			else if (id == token_name) {
				printf(LI_RED "%s" LI_DEF, str_val->c_str());
			}
			// Numeric literal.
			else {
				printf(LI_CYN "%lf" LI_DEF, num_val);
			}
		}
	};

	// Lexer state.
	//
	struct state {
		// Owning VM.
		//
		core::vm* L;

		// Current parser location.
		//
		std::string_view input = {};

		// Current line index.
		//
		size_t line = 1;

		// Current and lookahead token.
		//
		token_value                tok           = {};
		std::optional<token_value> tok_lookahead = {};

		// Last lexer error.
		//
		std::string last_error = {};

		// Initialized with a string view and a pointer to the VM for string interning.
		//
		state(core::vm* L, std::string_view input) : L(L), input(input), tok(scan()) {}
		state(std::string&&) = delete;

		// Default copy.
		//
		state(const state&)            = default;
		state& operator=(const state&) = default;

		// Error helper.
		//
		template<typename... Tx>
		token_value error(const char* fmt, Tx... args) {
			if (last_error.empty())
				last_error = util::fmt(fmt, args...);
			return token_value{.id = token_error};
		}

		// Scans for the next token.
		//
		token_value scan();

		// Checks and consumes a token.
		//
		token_value check(token tk) {
			if (tok.id != tk) {
				auto sv = cx_token_to_strv(tk);
				if (sv.empty())
					sv = {(const char*) &tk, 1};
				return this->error("expected token '%.*s'", sv.size(), sv.data());
			} else {
				return next();
			}
		}
		token_value check(char tk) { return check(token(tk)); }

		// Checks and consumes an optional token.
		//
		std::optional<token_value> opt(token tk) {
			if (tok.id == tk) {
				return next();
			}
			return std::nullopt;
		}
		std::optional<token_value> opt(char tk) { return opt(token(tk)); }

		// Gets the lookahead token.
		//
		token_value& lookahead() {
			LI_ASSERT_MSG("Double lookahead", !tok_lookahead);
			tok_lookahead = scan();
			return *tok_lookahead;
		}

		// Skips to the next token and returns the current one.
		//
		token_value next() {
			token_value result = std::move(tok);
			if (tok_lookahead) {
				tok = *std::exchange(tok_lookahead, std::nullopt);
			} else {
				tok = scan();
			}
			return result;
		}
	};
};