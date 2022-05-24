#include <array>
#include <lang/lexer.hpp>

namespace lightning {
	// Character traits.
	//
	enum char_trait : uint8_t {
		char_ctrl  = 1 << 0,
		char_punct = 1 << 1,
		char_space = 1 << 2,  // \t\v\f\20
		char_alpha = 1 << 3,
		char_eol   = 1 << 4,
		char_num   = 1 << 5,
		char_xnum  = 1 << 6,
		char_ident = 1 << 7,
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
		// Add EOL.
		result['\r'] |= char_eol;
		result['\n'] |= char_eol;
		// Add xnum.
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
	static constexpr bool is_ctrl(char c) {  return (char_traits[uint8_t(c)] & char_ctrl) != 0; }
	static constexpr bool is_punct(char c) { return (char_traits[uint8_t(c)] & char_punct) != 0; }
	static constexpr bool is_space(char c) { return (char_traits[uint8_t(c)] & char_space) != 0; }
	static constexpr bool is_eol(char c) {   return (char_traits[uint8_t(c)] & char_eol) != 0; }
	static constexpr bool is_alpha(char c) { return (char_traits[uint8_t(c)] & char_alpha) != 0; }
	static constexpr bool is_num(char c) {   return (char_traits[uint8_t(c)] & char_num) != 0; }
	static constexpr bool is_xnum(char c) {  return (char_traits[uint8_t(c)] & char_xnum) != 0; }
	static constexpr bool is_ident(char c) { return (char_traits[uint8_t(c)] & char_ident) != 0; }

	// String reader.
	//
	static token_value lexer_scan_str(lexer_state& state, bool is_long_str) {
		token_value result = {.id = token_string};
		result.num_val     = 0;
		return result;
	}

	// Numeric reader.
	//
	static token_value lexer_scan_num(lexer_state& state) { 
		token_value result = {.id = token_number};
		result.num_val     = 0;
		return result;
	}

	// Lexer scan.
	//
	static token_value lexer_scan(lexer_state& state) {
		while (!state.input.empty()) {
			char c = state.input.front();

			switch (c) {
				default:
					break;
			}
		}
	}

	// Initialized with a string buffer.
	//
	lexer_state::lexer_state(std::string data) : input_buffer(std::move(data)), input(input_buffer) {}

	// Gets the next/lookahead token.
	//
	token_value& lexer_state::next() {
		if (tok_lookahead) {
			tok_current = *tok_lookahead;
			tok_lookahead.reset();
		} else {
			tok_current = lexer_scan(*this);
		}
		return tok_current;
	}
	token_value& lexer_state::lookahead() {
		if (tok_lookahead) {
			// error: double look ahead
		}
		tok_lookahead = lexer_scan(*this);
		return *tok_lookahead;
	}
}