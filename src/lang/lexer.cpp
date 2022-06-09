#include <stdarg.h>
#include <array>
#include <cmath>
#include <lang/lexer.hpp>
#include <util/common.hpp>
#include <util/format.hpp>
#include <util/utf.hpp>

namespace li::lex {
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
		result['_'] |= char_ident;
		// Add spaces.
		result['\t'] |= char_space;
		result['\v'] |= char_space;
		result['\f'] |= char_space;
		result['\r'] |= char_space;  // hack!
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
	static constexpr bool is_punct(char c) { return (char_traits[uint8_t(c)] & char_punct) != 0; }
	static constexpr bool is_space(char c) { return (char_traits[uint8_t(c)] & char_space) != 0; }
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
	template<int Base>
	LI_INLINE static std::optional<int> parse_digit(std::string_view& value);

	static std::string escape_string(std::string_view str, std::string_view& err) {
		std::string result(str);

		// For each escape character found:
		//
		size_t n = 0;
		while (n < result.size()) {
			auto pos = result.find('\\', n);
			if (pos == std::string::npos) {
				break;
			}
			LI_ASSERT((pos + 1) != result.size());

#define SIMPLE_ESCAPE(x, y)                   \
	case x: {                                  \
		result[pos] = y;                        \
		result.erase(result.begin() + pos + 1); \
		n = pos + 1;                            \
		continue;                               \
	}

			switch (result[pos + 1]) {
				SIMPLE_ESCAPE('a', '\a')
				SIMPLE_ESCAPE('b', '\b')
				SIMPLE_ESCAPE('f', '\f')
				SIMPLE_ESCAPE('n', '\n')
				SIMPLE_ESCAPE('r', '\r')
				SIMPLE_ESCAPE('t', '\t')
				SIMPLE_ESCAPE('v', '\v')
				SIMPLE_ESCAPE('\\', '\\')
				SIMPLE_ESCAPE('"', '"')
				SIMPLE_ESCAPE('`', '`')
				SIMPLE_ESCAPE('\'', '\'')
				// \xAB
				case 'x': {
					// Parse 2 hexadecimal digits.
					//
					if ((pos + 4) > result.size()) {
						err = "invalid hex escape.";
						return {};
					}
					std::string_view src{result.data() + pos + 2, 2};
					auto             n1 = parse_digit<16>(src);
					auto             n0 = parse_digit<16>(src);
					if (!n1 || !n0) {
						err = "invalid hex escape.";
						return {};
					}

					// Insert-inplace.
					//
					uint8_t x = 0;
					x |= uint8_t(*n1) << (1 * 4);
					x |= uint8_t(*n0) << (0 * 4);
					result[pos] = (char) x;
					result.erase(result.begin() + pos + 1, result.begin() + pos + 4);
					n = pos + 1;
					break;
				}
				// \uABCD
				case 'u': {
					// Parse 4 hexadecimal digits.
					//
					if ((pos + 6) > result.size()) {
						err = "invalid unicode escape.";
						return {};
					}
					std::string_view src{result.data() + pos + 2, 4};
					auto             n3 = parse_digit<16>(src);
					auto             n2 = parse_digit<16>(src);
					auto             n1 = parse_digit<16>(src);
					auto             n0 = parse_digit<16>(src);
					if (!n3 || !n2 || !n1 || !n0) {
						err = "invalid unicode escape.";
						return {};
					}

					// Read as U16LE.
					//
					uint32_t cp = 0;
					cp |= uint32_t(*n3) << (3 * 4);
					cp |= uint32_t(*n2) << (2 * 4);
					cp |= uint32_t(*n1) << (1 * 4);
					cp |= uint32_t(*n0) << (0 * 4);

					// Insert-inplace.
					//
					static_assert(util::codepoint_cvt<char>::max_out <= 6, "can't write inplace");
					auto it = &result[pos];
					util::codepoint_cvt<char>::encode(cp, it);
					auto len = it - &result[pos];

					// Remove all after it.
					//
					result.erase(result.begin() + pos + len, result.begin() + pos + 6);
					n = pos + len + 1;
					break;
				}
				default:
					err = "invalid escape sequence.";
					return {};
			}
#undef SIMPLE_ESCAPE
		}
		return result;
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
	static token_value scan_fstr(state& state) {
		// Consume the quote.
		//
		auto& str = state.input;
		str.remove_prefix(1);

		// Until we escape all of it properly:
		//
		std::string      result{};
		std::string_view err{};
		while (!str.empty() && err.empty()) {
			auto next = str.find_first_of("\n`{");

			// Error if we reach EOL or EOF.
			if (next == std::string::npos) {
				return state.error("unmatched format string.");
			}
			if (str[next] == '\n') {
				return state.error("improperly terminated format string.");
			}

			// Break if we reached the end.
			//
			if (str[next] == '`') {
				result += escape_string(str.substr(0, next), err);
				str.remove_prefix(next + 1);
				break;
			}

			// Handle leftovers.
			//
			if (next != 0) {
				result += escape_string(str.substr(0, next), err);
				str.remove_prefix(next);
			}

			// Skip if escaped.
			//
			if (str.starts_with("{{")) {
				result += '{';
				str.remove_prefix(1);
				continue;
			}

			// Do not escape until we meet the end.
			//
			size_t it     = 1;
			char   in_str = 0;
			char   escape = 0;
			for (size_t debt = 1; debt > 0; it++) {
				if (it == str.size()) {
					return state.error("unmatched format string.");
				}

				// If within a string, and we see a '\', set the escape flag, if we see '\n' error.
				//
				if (!escape && in_str) {
					if (str[it] == '\n') {
						return state.error("improperly terminated format string.");
					}
					if (str[it] == '\\') {
						escape = in_str;
						continue;
					}
				}

				// If end of string, go back to block processing:
				//
				if (!escape && str[it] == in_str) {
					in_str = 0;
					continue;
				}
				escape = false;

				// If processing blocks:
				//
				if (!in_str) {
					if (str[it] == '"' || str[it] == '`') {
						in_str = str[it];
						continue;
					}
					if (str[it] == '{') {
						if ((it + 1) == str.size() || str[it + 1] != '{') {
							debt++;
						}
					} else if (str[it] == '}') {
						if ((it + 1) == str.size() || str[it + 1] != '}') {
							debt--;
						}
					}
				}
			}

			result += str.substr(0, it);
			str.remove_prefix(it);
		}

		// Propagate errors.
		//
		if (!err.empty()) {
			return state.error(err);
		}
		return {.id = token_fstr, .str_val = string::create(state.L, result)};
	}
	static token_value scan_str(state& state) {
		// Consume the quote.
		//
		state.input.remove_prefix(1);

		// TODO: Long string
		bool escape = false;
		for (size_t i = 0;; i++) {
			// If we reached EOF|EOL and there is no end of string, error.
			if (i == state.input.size() || state.input[i] == '\n') {
				return state.error("unfinished string.", state.line);
			}
			// If escaping next character, set the flag.
			else if (!escape && state.input[i] == '\\') {
				escape = true;
				continue;
			}
			// If not escaped end of string, return.
			else if (!escape && state.input[i] == '"') {
				std::string_view err = {};
				std::string      str = escape_string(state.input.substr(0, i), err);
				if (!err.empty()) {
					return state.error(err);
				}
				token_value result = {.id = token_lstr, .str_val = string::create(state.L, str)};
				state.input.remove_prefix(i + 1);
				return result;
			}

			// Clear escape.
			escape = false;
		}
	}
	static token_value scan_chr(state& state) {
		// Consume the quote.
		//
		state.input.remove_prefix(1);

		bool escape = false;
		for (size_t i = 0;; i++) {
			// If we reached EOF|EOL and there is no end of string, error.
			if (i == state.input.size() || state.input[i] == '\n') {
				return state.error("unfinished character literal.", state.line);
			}
			// If escaping next character, set the flag.
			else if (!escape && state.input[i] == '\\') {
				escape = true;
				continue;
			}
			// If not escaped end of string, return.
			else if (!escape && state.input[i] == '\'') {
				std::string_view err = {};
				std::string      str = escape_string(state.input.substr(0, i), err);
				if (!err.empty()) {
					return state.error(err);
				} else if (str.empty()) {
					return state.error("character literal empty.");
				} else if (str.size() != 1) {
					return state.error("character literal too long.");
				}
				token_value result = {.id = token_lnum, .num_val = number(str[0])};
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
			T           mul  = 1;
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
			if constexpr (Base < 15) {
				if (value.front() == 'e') {
					value.remove_prefix(1);
					T exponent = parse_digits<T, 10, false>(value);
					result *= (T) pow(Base, (double) exponent);
				}
			}
		}

		if (!value.empty()) {
			return state.error("Unexpected digit while parsing number: '%c'\n", value.front());
		}

		return {.id = token_lnum, .num_val = result};
	}

	template<int Base>
	static token_value parse_number(state& state) {
		// Fetch the integer part.
		//
		std::string_view integral_part = str_consume_all<char_alpha | char_num>(state.input);

		// If there is a fraction:
		//
		if (state.input.starts_with(".") && !state.input.starts_with("..")) {
			// Fetch the fraction part.
			//
			state.input.remove_prefix(1);
			std::string_view fractional_part = str_consume_all<char_alpha | char_num>(state.input);

			// Parse both sides and handle suffix.
			//
			number result = parse_digits<number, Base, false>(integral_part);
			if (!integral_part.empty())
				return state.error("Unexpected digit while parsing number: '%c'\n", integral_part.front());
			result += parse_digits<number, Base, true>(fractional_part);
			return parse_digits_handle_suffix<number, Base>(state, result, fractional_part);
		} else {
			// Parse the integral side and handle suffix.
			//
			number result = parse_digits<number, Base, false>(integral_part);
			return parse_digits_handle_suffix<number, Base>(state, result, integral_part);
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

	// Scans for the next token.
	//
	token_value state::scan() {
		while (!input.empty()) {
			char c = input.front();
			if (!c) {
				input = {};
				break;
			}

			// If whitespace, consume and continue.
			if (is_space(c)) {
				str_consume_all<char_space>(input);
				continue;
			}
			// If identifier, keyword or numeric literal.
			else if (is_ident(c)) {
				// Numeric literal.
				if (is_num(c)) {
					return scan_num(*this);
				}

				// Try matching against a keyword.
				auto word = str_consume_all<char_ident>(input);
				for (uint8_t i = token_name_min; i <= token_name_max; i++) {
					if (word == cx_token_to_strv(i)) {
						return {.id = token(i)};
					}
				}

				// Otherwise return as identifier.
				return {.id = token_name, .str_val = string::create(L, word)};
			}
			// If punctuation, try matching with a symbol.
			//
			else if (is_punct(c)) {
				// Handle all symbols:
				for (uint8_t i = token_sym_min; i <= token_sym_max; i++) {
					std::string_view sym = cx_token_to_strv(i);
					if (input.starts_with(sym)) {
						input.remove_prefix(sym.size());
						return {.id = token(i)};
					}
				}
			}

			switch (c) {
				// Newline:
				case '\n':
					line++;
					[[fallthrough]];
				// Whitespace:
				case '\t':
				case '\v':
				case '\f':
				case '\r':
					input.remove_prefix(1);
					continue;

				// Comment:
				case '#':
					input.remove_prefix(1);
					nextline(*this);
					continue;

				// Character literal:
				//
				case '\'': {
					return scan_chr(*this);
				}

				// String literal:
				case '`':
					return scan_fstr(*this);
				case '"':
					return scan_str(*this);

				// Finally, return as a single char token.
				default:
					input.remove_prefix(1);
					return {.id = token(c)};
			}
		}
		return {.id = token_eof};
	}
}