#include <array>
#include <lang/lexer.hpp>
#include <stdarg.h>
#include <util/common.hpp>
#include <util/format.hpp>

namespace lightning::lexer {
	// Character traits.
	//
	enum char_trait : uint8_t {
		char_ctrl  = 1 << 0,
		char_punct = 1 << 1,
		char_space = 1 << 2,  // \t\v\f\20
		char_alpha = 1 << 3,
		char_num   = 1 << 4,
		char_xnum  = 1 << 5,
		char_ident = 1 << 6,
	};
	static constexpr std::array<uint8_t, 256> char_traits = []() {
		std::array<uint8_t, 256> result = {0};

		// 00-1F: Control.
		for (size_t i = 0x00; i <= 0x1F; i++)
			result[i] = char_ctrl;
		// 20:    Space.
		result[0x20] = char_space;
		// 21-2F: Punctuation.
		for (size_t i = 0x21; i <= 0x2F; i++)
			result[i] = char_punct;
		// 30-39: Number + Identifier.
		for (size_t i = 0x30; i <= 0x39; i++)
			result[i] = char_num | char_ident;
		// 3A-40: Punctuation.
		for (size_t i = 0x3A; i <= 0x40; i++)
			result[i] = char_punct;
		// 41-5A: Alpha + Identifier.
		for (size_t i = 0x41; i <= 0x5A; i++)
			result[i] = char_alpha | char_ident;
		// 5B-60: Punctuation.
		for (size_t i = 0x5B; i <= 0x60; i++)
			result[i] = char_punct;
		// 61-7A: Alpha + Identifier.
		for (size_t i = 0x61; i <= 0x7A; i++)
			result[i] = char_alpha | char_ident;
		// 7B-7E: Punctuation.
		for (size_t i = 0x7B; i <= 0x7E; i++)
			result[i] = char_punct;
		// 7F:    Control.
		result[0x7F] = char_ctrl;
		// 80-FF: Identifier, yay unicode.
		for (size_t i = 0x80; i <= 0xFF; i++)
			result[i] = char_ident;

		// Add allowed identifiers.
		result['@'] |= char_ident;
		result['$'] |= char_ident;
		// Add spaces.
		result['\t'] |= char_space;
		result['\v'] |= char_space;
		result['\f'] |= char_space;
		result['\r'] |= char_space; // hack!
		// Add further num traits.
		for (size_t i = 'A'; i <= 'F'; i++)
			result[i] |= char_xnum;
		for (size_t i = 'a'; i <= 'f'; i++)
			result[i] |= char_xnum;
		for (size_t i = '0'; i <= '9'; i++)
			result[i] |= char_xnum;
		return result;
	}();

	// Define the helpers.
	//
	static constexpr bool is_ctrl(char c) { return (char_traits[uint8_t(c)] & char_ctrl) != 0; }
	static constexpr bool is_punct(char c) { return (char_traits[uint8_t(c)] & char_punct) != 0; }
	static constexpr bool is_space(char c) { return (char_traits[uint8_t(c)] & char_space) != 0; }
	static constexpr bool is_alpha(char c) { return (char_traits[uint8_t(c)] & char_alpha) != 0; }
	static constexpr bool is_num(char c) { return (char_traits[uint8_t(c)] & char_num) != 0; }
	static constexpr bool is_xnum(char c) { return (char_traits[uint8_t(c)] & char_xnum) != 0; }
	static constexpr bool is_ident(char c) { return (char_traits[uint8_t(c)] & char_ident) != 0; }

	template<uint8_t C>
	static constexpr std::string_view str_consume_all(std::string_view& s) {
		for (size_t i = 0; i != s.size(); i++) {
			if (!(char_traits[uint8_t(s[i])] & C)) {
				auto result = s.substr(0, i);
				s.remove_prefix(i);
				return result;
			}
		}
		return std::exchange(s, std::string_view{});
	}
	template<uint8_t C>
	static constexpr std::string_view str_consume_until(std::string_view& s) {
		for (size_t i = 0; i != s.size(); i++) {
			if (char_traits[uint8_t(s[i])] & C) {
				auto result = s.substr(0, i);
				s.remove_prefix(i);
				return result;
			}
		}
		return std::exchange(s, std::string_view{});
	}

	// Lexer error.
	//
	void error [[noreturn]] (const char* fmt, ...) {
		va_list args;
		va_start(args, fmt);
		vprintf(fmt, args);
		va_end(args);
		abort();
	}

	// Handles escapes within a string.
	//
	std::string escape( std::string_view str ) {
		// TODO:
		// unicode escape \u1234
		// escape \v \f \\ ...
		// 
		return std::string(str);
	}

	// Skips to next line.
	//
	static void nextline(state& state) {
		auto pos = state.input.find('\n');
		if (pos != std::string::npos) {
			state.line++;
			state.input.remove_prefix(pos + 1);
		} else {
			state.input = {};
		}
	}

	// String reader.
	//
	static token_value scan_str(state& state) {
		// Consume the quote.
		//
		state.input.remove_prefix(1);
		
		// TODO: Long string

		bool escape = false;
		for (size_t i = 0;; i++) {
			// If we reached EOF|EOL and there is no end of string, error.
			if (i == state.input.size() || state.input[i] == '\n') {
				return state.error("Unfinished string: line %d\n", state.line);
			} 
			// If escaping next character, set the flag.
			else if (state.input[i] == '\\') {
				escape = true;
				continue;
			} 
			// If not escaped end of string, return.
			else if (!escape && state.input[i] == '"') {
				token_value result = {.id = token_string, .str_val = state.input.substr(0, i)};
				state.input.remove_prefix(i + 1);
				return result;
			}
			
			// Clear escape.
			escape = false;
		}
	}

	// Number reader.
	//
	template<int Base>
	LI_INLINE static std::optional<int> parse_digit(std::string_view& value) {
		// Pop first character.
		char c = value.front();

		// If hex:
		if constexpr (Base == 16) {
			// Test validity.
			if (is_xnum(c)) {
				// Return value.
				c |= 0x20;
				value.remove_prefix(1);
				return (c & 0x40) ? (c - 'a' + 0xA) : (c - '0' + 0);
			}
		}
		// If decimal:
		else if constexpr (Base == 10) {
			// Test validity.
			if (is_num(c)) {
				value.remove_prefix(1);
				return c - '0' + 0;
			}
		}
		// If octal:
		else if constexpr (Base == 8) {
			// Test validity.
			if ('0' <= c && c <= '7') {
				value.remove_prefix(1);
				return c - '0' + 0;
			}
		}
		// If binary:
		else if constexpr (Base == 2) {
			// Test validity.
			if (c == '0' || c == '1') {
				value.remove_prefix(1);
				return c - '0' + 0;
			}
		}
		return std::nullopt;
	}
	template<typename T, int Base, bool Fraction>
	LI_INLINE static T parse_digits(std::string_view& value) {
		T result = 0;
		if constexpr (Fraction) {
			constexpr T Step = 1 / T(Base);
			T mul = 1;
			while (!value.empty()) {
				auto digit = parse_digit<Base>(value);
				if (!digit)
					return result;
				mul *= Step;
				result += *digit * mul;
			}
		} else {
			while (!value.empty()) {
				auto digit = parse_digit<Base>(value);
				if (!digit)
					return result;
				result *= (T) Base;
				result += *digit;
			}
		}
		return result;
	}

	template<typename T, int Base>
	LI_INLINE static token_value parse_digits_handle_suffix(state& state, T result, std::string_view& value) {
		if (!value.empty()) {
			if constexpr (Base == 10) {
				if (value.front() == 'e') {
					value.remove_prefix(1);
					T exponent = parse_digits<T, Base, false>(value);
					result *= pow(10, exponent);
				}
			}
		}

		if(!value.empty()) {
			return state.error("Unexpected digit while parsing number: '%c'\n", value.front());
		}

		if constexpr (std::is_floating_point_v<T>) {
			return {.id = token_number, .num_val = result};
		} else {
			return {.id = token_integer, .int_val = result};
		}
	}

	template<int Base>
	static token_value parse_number(state& state) {
		// Fetch the integer part.
		//
		std::string_view integral_part = str_consume_all<char_alpha | char_num>(state.input);

		// If there is a fraction:
		//
		if (state.input.starts_with(".")) {
			// Fetch the fraction part.
			//
			state.input.remove_prefix(1);
			std::string_view fractional_part = str_consume_all<char_alpha | char_num>(state.input);
			
			// Parse both sides and handle suffix.
			//
			double result = parse_digits<double, Base, false>(integral_part);
			if (!integral_part.empty())
				return state.error("Unexpected digit while parsing number: '%c'\n", integral_part.front());
			result +=       parse_digits<double, Base, true>(fractional_part);
			return parse_digits_handle_suffix<double, Base>(state, result, fractional_part);
		} else {
			// Parse the integral side and handle suffix.
			//
			int64_t result = parse_digits<int64_t, Base, false>(integral_part);
			return parse_digits_handle_suffix<int64_t, Base>(state, result, integral_part);
		}
	}

	static token_value scan_num(state& state) {
		// Switch based on the base.
		//
		if (state.input.starts_with("0x")) {
			state.input.remove_prefix(2);
			return parse_number<16>(state);
		} else if (state.input.starts_with("0o")) {
			state.input.remove_prefix(2);
			return parse_number<8>(state);
		} else if (state.input.starts_with("0b")) {
			state.input.remove_prefix(2);
			return parse_number<2>(state);
		} else {
			return parse_number<10>(state);
		}
	}

	// Lexer scan.
	//
	static token_value scan(state& state) {
		while (!state.input.empty()) {
			char c = state.input.front();

			// If whitespace, consume and continue.
			if (is_space(c)) {
				str_consume_all<char_space>(state.input);
				continue;
			}
			// If identifier, keyword or numeric literal.
			else if (is_ident(c)) {
				// Numeric literal.
				if (is_num(c)) {
					return scan_num(state);
				}
				
				// Try matching against a keyword.
				auto word = str_consume_all<char_ident>(state.input);
				for (uint8_t i = token_name_min; i <= token_name_max; i++) {
					if (word == cx_token_to_strv(i)) {
						return {.id = token(i)};
					}
				}
				
				// Otherwise return as identifier.
				return {.id = token_name, .str_val = word};
			}
			// If punctuation, try matching with a symbol.
			//
			else if (is_punct(c)) {
				// Handle all symbols:
				for (uint8_t i = token_sym_min; i <= token_sym_max; i++) {
					std::string_view sym = cx_token_to_strv(i);
					if (state.input.starts_with(sym)) {
						state.input.remove_prefix(sym.size());
						return {.id = token(i)};
					}
				}
			}

			switch (c) {
				// Newline:
				case '\n':
					state.line++;
					[[fallthrough]];
				// Whitespace:
				case '\t':
				case '\v':
				case '\f':
				case '\r': 
					state.input.remove_prefix(1);
					continue;

				// Comment:
				case '#':
					state.input.remove_prefix(1);
					nextline(state);
					continue;

				// String literal:
				case '"':
					return scan_str(state);

				// Finally, return as a single char token.
				default:
					state.input.remove_prefix(1);
					return {.id = token(c)};
			}
		}
		return {.id = token_eof};
	}

	// Initialized with a string buffer.
	//
	state::state(std::string data) : input_buffer(std::move(data)), input(input_buffer) {}

	// Gets the next/lookahead token.
	//
	token_value& state::next() {
		if (tok_lookahead) {
			tok_current = *tok_lookahead;
			tok_lookahead.reset();
		} else {
			tok_current = scan(*this);
		}
		return tok_current;
	}
	token_value& state::lookahead() {
		LI_ASSERT_MSG("Double lookahead", !tok_lookahead);
		tok_lookahead = scan(*this);
		return *tok_lookahead;
	}
}