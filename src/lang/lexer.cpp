#if (defined(__linux__) || defined(__EMSCRIPTEN__)) && !defined(_GNU_SOURCE)
	#define _GNU_SOURCE
#endif

#include <stdarg.h>
#include <stdlib.h>
#include <array>
#if !defined(__EMSCRIPTEN__)
	#include <charconv>
#endif
#include <locale.h>
#include <limits>
#if defined(__APPLE__)
	#include <xlocale.h>
#endif
#include <cmath>
#include <lang/lexer.hpp>
#include <util/common.hpp>
#include <util/format.hpp>
#include <util/utf.hpp>
#include <vector>
#include <vm/rc.hpp>
#include <vm/state.hpp>

namespace li::lex {
	// Shared by copied lexer states. Each entry is the one owned reference
	// returned by string::create; token_value instances only borrow it.
	//
	struct token_string_arena {
		vm*                  L;
		std::vector<string*> values;

		explicit token_string_arena(vm* L) : L(L) {}

		~token_string_arena() {
			for (string* value : values) {
				rc::release(L, value);
			}
		}

		string* adopt(string* value) {
			try {
				values.push_back(value);
			} catch (...) {
				rc::release(L, value);
				throw;
			}
			return value;
		}
	};

	state::state(vm* owner, std::string_view source, std::string_view name)
		 : L(owner),
			token_strings(std::make_shared<token_string_arena>(owner)),
			source(source),
			input(source),
			source_name(name),
			verbose_errors(owner->verbose_errors) {
		tok = scan();
	}

	string* state::make_token_string(std::string_view value) { return token_strings->adopt(string::create(L, value)); }

	static msize_t source_column(std::string_view source, std::string_view remaining) {
		const uintptr_t source_begin = reinterpret_cast<uintptr_t>(source.data());
		const uintptr_t source_end   = source_begin + source.size();
		const uintptr_t position     = reinterpret_cast<uintptr_t>(remaining.data());
		if (position < source_begin || position > source_end)
			return 1;

		const size_t offset     = size_t(position - source_begin);
		const size_t line_break = offset ? source.rfind('\n', offset - 1) : std::string_view::npos;
		return msize_t(offset - (line_break == std::string_view::npos ? 0 : line_break + 1) + 1);
	}

	static std::optional<std::string_view> source_excerpt(std::string_view source, msize_t line) {
		if (line == 0)
			return std::nullopt;

		size_t begin = 0;
		for (msize_t current = 1; current < line; ++current) {
			const size_t newline = source.find('\n', begin);
			if (newline == std::string_view::npos)
				return std::nullopt;
			begin = newline + 1;
		}
		size_t end = source.find('\n', begin);
		if (end == std::string_view::npos)
			end = source.size();
		if (end > begin && source[end - 1] == '\r')
			--end;
		return source.substr(begin, end - begin);
	}

	token_value state::diagnostic_token() const {
		if (scan_active) {
			token_value  result   = scan_token;
			const size_t consumed = scan_input.size() >= input.size() ? scan_input.size() - input.size() : 0;
			if (consumed)
				result.length = msize_t(consumed);
			return result;
		}
		// EOF has no span of its own; a keyword that starts a later line belongs
		// to the next statement. Both anchor at the last consumed token.
		if (last_token_line && (tok.id == token_eof || (tok.source_line > last_token.end_line && is_token_keyword(tok.id))))
			return last_token;
		return tok;
	}

	token_value state::report_error(const token_value& at, std::string_view message) {
		if (last_error.empty()) {
			last_error = util::fmt("[%.*s:%u:%u] %.*s", int(source_name.size()), source_name.data(), unsigned(at.source_line), unsigned(at.source_column),
				 int(message.size()), message.data());

			if (auto excerpt = source_excerpt(source, at.source_line)) {
				last_error.push_back('\n');
				last_error.append(*excerpt);
				last_error.push_back('\n');

				const size_t column = std::min<size_t>(at.source_column ? at.source_column - 1 : 0, excerpt->size());
				for (size_t i = 0; i != column; ++i)
					last_error.push_back((*excerpt)[i] == '\t' ? '\t' : ' ');
				last_error.push_back('^');

				size_t underline = std::max<size_t>(at.length, 1);
				underline        = std::min(underline, excerpt->size() > column ? excerpt->size() - column : size_t(1));
				last_error.append(underline - 1, '~');
			}

			if (!frames.empty()) {
				if (verbose_errors) {
					for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
						last_error += "\n  note: ";
						last_error += it->label;
					}
				} else {
					last_error += "\n  note: ";
					last_error += frames.back().label;
				}
			}
		}

		token_value result = at;
		result.id          = token_error;
		return result;
	}

	token_value state::error_at(const token_value& at, const char* fmt, ...) {
		va_list args;
		va_start(args, fmt);
		va_list copy;
		va_copy(copy, args);
		const int size = vsnprintf(nullptr, 0, fmt, copy);
		va_end(copy);

		std::string message;
		if (size > 0) {
			message.resize(size_t(size) + 1);
			vsnprintf(message.data(), message.size(), fmt, args);
			message.pop_back();
		}
		va_end(args);
		return report_error(at, message);
	}

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
	void error [[noreturn]](const char* fmt, ...) {
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

	struct long_bracket_span {
		size_t content_end;
		size_t consumed;
		size_t lines;
	};

	// Returns the number of '=' characters in a long-bracket opener.
	//
	static std::optional<size_t> long_bracket_separator(std::string_view input) {
		if (input.size() < 2 || input.front() != '[') {
			return std::nullopt;
		}

		size_t cursor = 1;
		while (cursor < input.size() && input[cursor] == '=') {
			cursor++;
		}
		if (cursor == input.size() || input[cursor] != '[') {
			return std::nullopt;
		}
		return cursor - 1;
	}

	// Finds the exact matching delimiter without treating nested or mismatched
	// delimiter text specially.
	//
	static std::optional<long_bracket_span> find_long_bracket(std::string_view input, size_t separator) {
		const size_t content_begin = separator + 2;
		size_t       lines         = 0;
		for (size_t cursor = content_begin; cursor < input.size(); cursor++) {
			if (input[cursor] == '\n') {
				lines++;
				continue;
			}
			if (input[cursor] != ']') {
				continue;
			}

			size_t delimiter = cursor + 1;
			size_t equals    = 0;
			while (delimiter < input.size() && input[delimiter] == '=') {
				delimiter++;
				equals++;
			}
			if (equals == separator && delimiter < input.size() && input[delimiter] == ']') {
				return long_bracket_span{
					 .content_end = cursor,
					 .consumed    = delimiter + 1,
					 .lines       = lines,
				};
			}
		}
		return std::nullopt;
	}

	// Verbatim literals keep their line breaks; a CRLF sequence from a Windows
	// checkout means the same source text as LF, so it is normalized.
	static std::string normalize_line_breaks(std::string_view text) {
		std::string result;
		result.reserve(text.size());
		for (size_t i = 0; i != text.size(); i++) {
			if (text[i] == '\r' && i + 1 != text.size() && text[i + 1] == '\n')
				continue;
			result.push_back(text[i]);
		}
		return result;
	}

	static token_value scan_long_string(state& state, size_t separator) {
		auto span = find_long_bracket(state.input, separator);
		if (!span) {
			return state.error("unterminated long string.");
		}

		const size_t content_begin = separator + 2;
		string*      value         = state.make_token_string(normalize_line_breaks(state.input.substr(content_begin, span->content_end - content_begin)));
		state.line += span->lines;
		state.input.remove_prefix(span->consumed);
		return {.id = token_lstr, .str_val = value};
	}

	static std::optional<size_t> raw_string_separator(std::string_view input) {
		if (input.empty() || input.front() != 'r')
			return std::nullopt;
		size_t cursor = 1;
		while (cursor < input.size() && input[cursor] == '#')
			++cursor;
		if (cursor == input.size() || input[cursor] != '"')
			return std::nullopt;
		return cursor - 1;
	}

	static std::optional<long_bracket_span> find_raw_string(std::string_view input, size_t separator) {
		size_t lines = 0;
		for (size_t cursor = separator + 2; cursor < input.size(); ++cursor) {
			if (input[cursor] == '\n')
				++lines;
			if (input[cursor] != '"')
				continue;
			size_t suffix = 0;
			while (suffix < separator && cursor + 1 + suffix < input.size() && input[cursor + 1 + suffix] == '#')
				++suffix;
			if (suffix == separator)
				return long_bracket_span{cursor, cursor + 1 + suffix, lines};
		}
		return std::nullopt;
	}

	static std::optional<long_bracket_span> find_block_comment(std::string_view input) {
		size_t depth = 1;
		size_t lines = 0;
		for (size_t cursor = 2; cursor < input.size(); ++cursor) {
			if (input[cursor] == '\n')
				++lines;
			if (cursor + 1 == input.size())
				break;
			if (input[cursor] == '/' && input[cursor + 1] == '*') {
				++depth;
				++cursor;
			} else if (input[cursor] == '*' && input[cursor + 1] == '/') {
				if (--depth == 0)
					return long_bracket_span{cursor, cursor + 2, lines};
				++cursor;
			}
		}
		return std::nullopt;
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
					auto remaining = str.substr(it);
					if (auto separator = raw_string_separator(remaining)) {
						auto span = find_raw_string(remaining, *separator);
						if (!span)
							return state.error("unterminated raw string in interpolation.");
						state.line += span->lines;
						it += span->consumed - 1;
						continue;
					}
					if (remaining.starts_with("/*")) {
						auto span = find_block_comment(remaining);
						if (!span)
							return state.error("unterminated block comment in interpolation.");
						state.line += span->lines;
						it += span->consumed - 1;
						continue;
					}
					if (remaining.starts_with("//")) {
						auto end = remaining.find('\n');
						if (end == std::string_view::npos)
							return state.error("unmatched format string.");
						it += end - 1;
						continue;
					}
					if (str[it] == '\n')
						++state.line;
					if (str[it] == '"' || str[it] == '`' || str[it] == '\'') {
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
		return {.id = token_fstr, .str_val = state.make_token_string(result)};
	}
	static token_value scan_str(state& state) {
		// Consume the quote.
		//
		state.input.remove_prefix(1);

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
				token_value result = {.id = token_lstr, .str_val = state.make_token_string(str)};
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
	struct c_numeric_locale {
#if LI_WINDOWS
		_locale_t value = _create_locale(LC_NUMERIC, "C");

		~c_numeric_locale() {
			if (value)
				_free_locale(value);
		}
#else
		locale_t value = newlocale(LC_NUMERIC_MASK, "C", nullptr);

		~c_numeric_locale() {
			if (value)
				freelocale(value);
		}
#endif
	};

	static bool parse_decimal_c_locale(std::string_view literal, number& value) {
		std::array<char, 128> local;
		std::string           allocated;
		const char*           text;

		if (literal.size() < local.size()) {
			memcpy(local.data(), literal.data(), literal.size());
			local[literal.size()] = '\0';
			text                  = local.data();
		} else {
			allocated.assign(literal);
			text = allocated.c_str();
		}

		static const c_numeric_locale locale;
		if (!locale.value)
			return false;

		char* end = nullptr;
#if LI_WINDOWS
		value = _strtod_l(text, &end, locale.value);
#else
		value = strtod_l(text, &end, locale.value);
#endif
		return end == text + literal.size();
	}

	static bool parse_decimal(std::string_view literal, number& value) {
#if LI_EMSCRIPTEN || defined(_LIBCPP_VERSION)
		// Some supported libc++ releases do not provide floating-point
		// from_chars. Their C-locale conversion is correctly rounded.
		return parse_decimal_c_locale(literal, value);
#else
		const char* end    = literal.data() + literal.size();
		auto        parsed = std::from_chars(literal.data(), end, value, std::chars_format::general);
		if (parsed.ec == std::errc::result_out_of_range)
			return parse_decimal_c_locale(literal, value);
		return parsed.ec == std::errc{} && parsed.ptr == end;
#endif
	}

	static std::optional<numeric_kind> parse_numeric_suffix(std::string_view suffix) {
		if (suffix.empty())
			return numeric_kind::number;
		if (suffix == "i8")
			return numeric_kind::i8;
		if (suffix == "i16")
			return numeric_kind::i16;
		if (suffix == "i32")
			return numeric_kind::i32;
		if (suffix == "i64")
			return numeric_kind::i64;
		if (suffix == "u8")
			return numeric_kind::u8;
		if (suffix == "u16")
			return numeric_kind::u16;
		if (suffix == "u32" || suffix == "u")
			return numeric_kind::u32;
		if (suffix == "u64")
			return numeric_kind::u64;
		if (suffix == "f32")
			return numeric_kind::f32;
		if (suffix == "f64")
			return numeric_kind::f64;
		return std::nullopt;
	}

	static bool append_decimal_digits(std::string& normalized, std::string_view source, size_t& end) {
		bool saw_digit = false;
		bool separator = false;
		while (end < source.size() && (is_num(source[end]) || source[end] == '\'')) {
			if (source[end] == '\'') {
				if (!saw_digit || separator || end + 1 == source.size() || !is_num(source[end + 1]))
					return false;
				separator = true;
			} else {
				normalized.push_back(source[end]);
				saw_digit = true;
				separator = false;
			}
			end++;
		}
		return saw_digit && !separator;
	}

	static token_value scan_decimal_number(state& state) {
		const std::string_view source = state.input;
		std::string            literal;
		size_t                 end = 0;
		if (!append_decimal_digits(literal, source, end)) {
			state.input.remove_prefix(std::max<size_t>(end, 1));
			return state.error("Invalid numeric separator placement.");
		}

		bool fractional = false;
		if (end < source.size() && source[end] == '.' && !source.substr(end).starts_with("..")) {
			fractional = true;
			literal.push_back('.');
			end++;
			if (!append_decimal_digits(literal, source, end)) {
				state.input.remove_prefix(end);
				return state.error("Expected a digit after the decimal point.");
			}
		}

		bool exponent = false;
		if (end < source.size() && (source[end] == 'e' || source[end] == 'E')) {
			exponent = true;
			literal.push_back('e');
			end++;
			if (end < source.size() && (source[end] == '+' || source[end] == '-'))
				literal.push_back(source[end++]);
			if (!append_decimal_digits(literal, source, end)) {
				state.input.remove_prefix(end);
				return state.error("Expected a digit in the numeric exponent.");
			}
		}

		const size_t suffix_begin = end;
		while (end < source.size() && (char_traits[uint8_t(source[end])] & (char_alpha | char_num)))
			end++;
		auto suffix = source.substr(suffix_begin, end - suffix_begin);
		auto kind   = parse_numeric_suffix(suffix);
		state.input.remove_prefix(end);
		if (!kind)
			return state.error("Unexpected numeric literal suffix '%.*s'.", int(suffix.size()), suffix.data());
		if ((fractional || exponent) && *kind >= numeric_kind::i8 && *kind <= numeric_kind::u64)
			return state.error("Integer suffix is not valid on a fractional literal.");

		number value;
		if (!parse_decimal(literal, value))
			return state.error("Invalid decimal literal.");
		if (*kind >= numeric_kind::i8 && *kind <= numeric_kind::u64) {
			if (!std::isfinite(value) || std::trunc(value) != value)
				return state.error("Integer literal must have an integral finite value.");
		} else if (*kind == numeric_kind::f32 && std::isfinite(value) && std::abs(value) > std::numeric_limits<float>::max()) {
			return state.error("Floating-point literal is out of range for f32.");
		}
		return {.id = token_lnum, .num_kind = *kind, .num_val = value};
	}

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
			return scan_decimal_number(state);
		}
	}

	// Scans for the next token.
	//
	token_value state::scan() {
		scan_active = true;
		auto stamp  = [&](token_value value, msize_t source_line) {
			value.source_line   = source_line;
			value.source_column = scan_token.source_column;
			value.length        = msize_t(scan_input.size() >= input.size() ? scan_input.size() - input.size() : 0);
			if (value.id != token_eof && value.length == 0)
				value.length = std::max<msize_t>(scan_token.length, 1);
			value.end_line = line;
			scan_active    = false;
			return value;
		};

		while (!input.empty()) {
			scan_input               = input;
			scan_token               = {};
			scan_token.source_line   = line;
			scan_token.source_column = source_column(source, input);
			const size_t first_line  = input.find('\n');
			scan_token.length        = msize_t(first_line == std::string_view::npos ? input.size() : first_line);
			char c                   = input.front();
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
				const msize_t source_line = line;
				if (auto separator = raw_string_separator(input)) {
					auto span = find_raw_string(input, *separator);
					if (!span)
						return stamp(error("unterminated raw string."), source_line);
					const size_t content_begin = *separator + 2;
					auto*        value         = make_token_string(normalize_line_breaks(input.substr(content_begin, span->content_end - content_begin)));
					line += span->lines;
					input.remove_prefix(span->consumed);
					return stamp({.id = token_lstr, .str_val = value}, source_line);
				}

				// Numeric literal.
				if (is_num(c)) {
					return stamp(scan_num(*this), source_line);
				}

				// Try matching against a keyword.
				auto word = str_consume_all<char_ident>(input);
				for (uint8_t i = token_name_min; i <= token_name_max; i++) {
					if (word == cx_token_to_strv(i)) {
						return stamp({.id = token(i)}, source_line);
					}
				}

				// Otherwise return as identifier.
				return stamp({.id = token_name, .str_val = make_token_string(word)}, source_line);
			}
			// If punctuation, try matching with a symbol.
			//
			else if (is_punct(c)) {
				if (input.starts_with("//")) {
					nextline(*this);
					continue;
				}
				if (input.starts_with("/*")) {
					const msize_t source_line = line;
					auto          span        = find_block_comment(input);
					if (!span)
						return stamp(error("unterminated block comment."), source_line);
					line += span->lines;
					input.remove_prefix(span->consumed);
					continue;
				}
				// Handle all symbols:
				for (uint8_t i = token_sym_min; i <= token_sym_max; i++) {
					std::string_view sym = cx_token_to_strv(i);
					if (input.starts_with(sym)) {
						const msize_t source_line = line;
						input.remove_prefix(sym.size());
						return stamp({.id = token(i)}, source_line);
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

				// Pack counts use `#Name...`; every other `#` form remains a comment.
				case '#':
					if (input.size() > 1 && is_ident(input[1]) && !is_num(input[1])) {
						const msize_t source_line = line;
						input.remove_prefix(1);
						return stamp({.id = token('#')}, source_line);
					}
					input.remove_prefix(1);
					if (auto separator = long_bracket_separator(input)) {
						auto span = find_long_bracket(input, *separator);
						if (!span) {
							return stamp(error("unterminated long comment."), line);
						}
						line += span->lines;
						input.remove_prefix(span->consumed);
					} else {
						nextline(*this);
					}
					continue;

				// Character literal:
				//
				case '\'': {
					const msize_t source_line = line;
					return stamp(scan_chr(*this), source_line);
				}

				// String literal:
				case '`': {
					const msize_t source_line = line;
					return stamp(scan_fstr(*this), source_line);
				}
				case '"': {
					const msize_t source_line = line;
					return stamp(scan_str(*this), source_line);
				}
				case '[': {
					// `[[` opens an attribute list; long strings need at least one
					// separator (`[=[ ... ]=]`). Long comments keep the zero form.
					const msize_t source_line = line;
					if (auto separator = long_bracket_separator(input); separator && *separator != 0) {
						return stamp(scan_long_string(*this, *separator), source_line);
					}
					input.remove_prefix(1);
					return stamp({.id = token('[')}, source_line);
				}

				// Finally, return as a single char token.
				default: {
					const msize_t source_line = line;
					input.remove_prefix(1);
					return stamp({.id = token(c)}, source_line);
				}
			}
		}
		scan_input               = input;
		scan_token               = {};
		scan_token.source_line   = line;
		scan_token.source_column = source_column(source, input);
		return stamp({.id = token_eof}, line);
	}
}