#pragma once
#include <stdio.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <util/format.hpp>
#include <vector>
#include <vm/string.hpp>
#include <vm/types.hpp>

namespace li {
	struct vm;
};

namespace li::lex {
	struct token_string_arena;

	// Token enumerator:
	//  _    => keyword        | "let"
	//  __   => symbol mapped  | "--"
	//  ___  => single char    | '+'
	//  ____ => literal        | <string>
	//
#define LIGHTNING_ENUM_TOKENS(_, __, ___, ____)                                      \
	/* Logical operators */                                                           \
		__(land, &&) __(lor, ||) ___(lnot, '!') __(eq, ==) __(ne, !=)			    \
		___(lt, '<') ___(gt, '>') __(le, <=) __(ge, >=)						          \
		/* Arithmetic operators */															    \
		___(add, '+') ___(sub, '-') ___(mul, '*') ___(div, '/') 					    \
		___(mod, '%') ___(pow, '^')															 \
		/* Ternary operator */																    \
		___(tif, '?') ___(telse, ':')														    \
		/* Compound operators */															    \
		__(cadd, +=) __(csub, -=) __(cmul, *=) __(cdiv, /=) __(cmod, %=)         \
		__(cpow, ^=) __(cnullc, ??=) __(cinc, ++) __(cdec, --)                   \
		/* Language operators */															    \
		__(dots, ...) __(rangei, ..=) __(range, ..) __(nullc, ??)                \
		__(icall, ->) __(ucall, ::) __(idxlif, ?.) __(idxif, ?[) __(fatarrow, =>) \
		/* Literal tokens */																	    \
		____(eof, <eof>) ____(lnum, <number>) ____(name, <name>)                 \
		____(lstr, <string>) ____(fstr, <fstring>) ____(error, <error>) 		    \
		/* Keywords */																			    \
		_(true) _(false) _(nil)  _(let) _(const) _(if) _(else) _(while) _(for) _(loop)  \
		_(break) _(continue) _(try) _(catch) _(throw) _(return) _(in) _(is)      \
		_(bool) _(number) _(table) _(array) _(string) _(object) _(class) _(function)    \
		_(fn) _(struct) _(type) _(export) _(import) _(as) _(dyn) _(match) _(leave) _(yield) _(delete) _(defer)

	// Token identifiers.
	//
	enum token : uint8_t {
#define TK_CHAR(name, chr) token_##name = chr,
#define TK_NAME(name, ...) token_##name,
#define TK_RET(name, ...)  return token_##name;

		// Character tokens, end at 0x7F.
		LIGHTNING_ENUM_TOKENS(LI_NOOP, LI_NOOP, TK_CHAR, LI_NOOP) token_char_max = 0x7F,

		// Symbolic tokens.
		LIGHTNING_ENUM_TOKENS(LI_NOOP, TK_NAME, LI_NOOP, LI_NOOP)
		// Named tokens.
		LIGHTNING_ENUM_TOKENS(TK_NAME, LI_NOOP, LI_NOOP, LI_NOOP)
		// Literal tokens.
		LIGHTNING_ENUM_TOKENS(LI_NOOP, LI_NOOP, LI_NOOP, TK_NAME)

			 token_lit_max_plus_one,
		token_lit_max  = token_lit_max_plus_one - 1,
		token_sym_min  = token_char_max + 1,
		token_name_min = []() { LIGHTNING_ENUM_TOKENS(TK_RET, LI_NOOP, LI_NOOP, LI_NOOP); }(),
		token_lit_min = []() { LIGHTNING_ENUM_TOKENS(LI_NOOP, LI_NOOP, LI_NOOP, TK_RET); }(),
		token_sym_max  = token_name_min,
		token_name_max = token_lit_min,

#undef TK_RET
#undef TK_CHAR
#undef TK_NAME
	};

	// Token traits.
	//
	LI_INLINE static constexpr bool is_token_literal(uint8_t t) { return token_lit_min <= t && t <= token_lit_max; }
	LI_INLINE static constexpr bool is_token_symbolic(uint8_t t) { return token_sym_min <= t && t < token_sym_max; }
	LI_INLINE static constexpr bool is_token_keyword(uint8_t t) { return token_name_min <= t && t < token_name_max; }
	LI_INLINE static constexpr bool is_token_character(uint8_t t) { return t <= token_char_max; }
	LI_INLINE static constexpr bool is_token_complex(uint8_t t) { return token_sym_min <= t && t <= token_lit_max; }

	// Complex token to string conversion.
	//
	static constexpr std::string_view cx_token_to_str_map[] = {
#define TK_SYM(name, ...)  #name,
#define TK_NAME(name, sym) #sym,

		 // Symbolic tokens.
		 LIGHTNING_ENUM_TOKENS(LI_NOOP, TK_NAME, LI_NOOP, LI_NOOP)
		 // Named tokens.
		 LIGHTNING_ENUM_TOKENS(TK_SYM, LI_NOOP, LI_NOOP, LI_NOOP)
		 // Literal tokens.
		 LIGHTNING_ENUM_TOKENS(LI_NOOP, LI_NOOP, LI_NOOP, TK_NAME)

#undef TK_SYM
#undef TK_NAME
	};
	static constexpr std::string_view cx_token_to_strv(uint8_t tk) {
		if (is_token_complex(tk)) {
			return cx_token_to_str_map[uint8_t(tk) - token_sym_min];
		}
		return {};
	}

	// Numeric literal spelling retained for strict-mode inference. Dynamic code
	// continues to observe the ordinary binary64 value.
	//
	enum class numeric_kind : uint8_t {
		number,
		i8,
		i16,
		i32,
		i64,
		u8,
		u16,
		u32,
		u64,
		f32,
		f64,
	};

	// Token value.
	//
	struct token_value {
		// Identifier and source span.
		//
		token        id            = token_eof;
		msize_t      source_line   = 1;
		msize_t      source_column = 1;
		msize_t      length        = 0;
		msize_t      end_line      = 1;
		numeric_kind num_kind      = numeric_kind::number;

		// Value.
		//
		union {
			string* str_val;  // token_lstr, token_fstr, token_name
			number  num_val;  // token_lnum
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
			} else if (id == token_eof) {
				return "<EOF>";
			}
			// Named/Symbolic token.
			else if (id < token_lit_min) {
				return std::string(cx_token_to_strv(id));
			}
			// String literal.
			else if (id == token_lstr) {
				return util::fmt("\"%s\"", str_val->c_str());
			} else if (id == token_fstr) {
				return util::fmt("`%s`", str_val->c_str());
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
			} else if (id == token_fstr) {
				printf(LI_BLU "`%s`" LI_DEF, str_val->c_str());
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

	struct diagnostic_frame {
		std::string label;
		msize_t     line;
	};

	// Lexer state.
	//
	struct state {
		// Owning VM.
		//
		vm* L;

	  private:
		// Token strings are borrowed by the parser and shared by lexer snapshots.
		// The final related state releases every string allocation made while
		// scanning that state family.
		//
		std::shared_ptr<token_string_arena> token_strings;
		std::string_view                    source      = {};
		bool                                scan_active = false;
		token_value                         scan_token  = {};
		std::string_view                    scan_input  = {};

		token_value diagnostic_token() const;
		token_value report_error(const token_value& at, std::string_view message);

	  public:
		// Current parser location.
		//
		std::string_view input = {};

		// Current line index and source name.
		//
		std::string_view source_name = {};
		msize_t          line        = 1;

		// Current and lookahead token.
		//
		token_value                tok           = {};
		std::optional<token_value> tok_lookahead = {};

		// Last lexer error and the end line of the most recently consumed token.
		//
		std::string                   last_error      = {};
		msize_t                       last_token_line = 0;
		token_value                   last_token      = {};
		std::vector<diagnostic_frame> frames          = {};
		bool                          verbose_errors  = false;

		// Initialized with a string view and a pointer to the VM for string interning.
		//
		state(vm* L, std::string_view input, std::string_view name = {});
		state(std::string&&) = delete;

		// Copies and moves share token ownership so lookahead/recovery snapshots
		// cannot invalidate each other's borrowed token values.
		//
		state(const state&)                = default;
		state(state&&) noexcept            = default;
		state& operator=(const state&)     = default;
		state& operator=(state&&) noexcept = default;

		// Creates an owned VM string, adopts it into the shared token arena, and
		// returns a borrow valid for the lifetime of any related lexer state.
		//
		string* make_token_string(std::string_view value);

		// Error helper.
		//
		token_value error_at(const token_value& at, const char* fmt, ...);

		template<typename... Tx>
		token_value error(const char* fmt, Tx... args) {
			return report_error(diagnostic_token(), util::fmt(fmt, args...));
		}
		token_value error(std::string_view err) { return report_error(diagnostic_token(), err); }

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
				return this->error("expected token '%.*s', got '%s'", sv.size(), sv.data(), tok.to_string().c_str());
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
			if (!tok_lookahead)
				tok_lookahead = scan();
			return *tok_lookahead;
		}

		// Skips to the next token and returns the current one.
		//
		token_value next() {
			token_value result = std::move(tok);
			last_token         = result;
			last_token_line    = result.end_line;
			if (tok_lookahead) {
				tok = *std::exchange(tok_lookahead, std::nullopt);
			} else {
				tok = scan();
			}
			return result;
		}
	};
};
