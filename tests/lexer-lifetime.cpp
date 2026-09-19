#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <lang/lexer.hpp>
#include <string_view>
#include <utility>
#include <vm/state.hpp>

namespace {
	using namespace li;

	[[noreturn]] void fail(const char* message) {
		std::fprintf(stderr, "lexer lifetime regression: %s\n", message);
		std::abort();
	}

	void require(bool condition, const char* message) {
		if (!condition)
			fail(message);
	}

	void require_live(vm* L, uint64_t expected, const char* phase) {
		if (L->gc.live_objects == expected)
			return;
		std::fprintf(stderr, "lexer lifetime regression: %s left %llu live objects, expected %llu\n", phase, static_cast<unsigned long long>(L->gc.live_objects),
			 static_cast<unsigned long long>(expected));
		std::abort();
	}

	void copied_lookahead_lifetime(vm* L) {
		static constexpr std::string_view source   = R"li(alpha "line\nvalue" [==[long
value]==] `prefix {beta}` #[=[ignored
comment]=] match leave step => omega)li";
		const uint64_t                    baseline = L->gc.live_objects;

		for (unsigned iteration = 0; iteration != 64; ++iteration) {
			{
				lex::state lexer(L, source, "lexer-lifetime");
				require(lexer.tok == lex::token_name, "initial identifier was not scanned");
				string* alpha = lexer.tok.str_val;
				require(alpha->view() == "alpha", "initial identifier contents were corrupted");

				lex::state       checkpoint = lexer;
				lex::token_value first      = lexer.next();
				require(first == lex::token_name && first.str_val == alpha, "next did not preserve the borrowed identifier");
				require(lexer.tok == lex::token_lstr, "escaped string was not scanned");
				string* escaped = lexer.tok.str_val;
				require(escaped->view() == "line\nvalue", "escaped string contents were corrupted");

				lex::token_value& ahead = lexer.lookahead();
				require(ahead == lex::token_lstr, "long string lookahead was not scanned");
				string* long_string = ahead.str_val;
				require(long_string->view() == "long\nvalue", "long string contents were corrupted");
				lex::state with_lookahead = lexer;

				require(lexer.next() == lex::token_lstr, "current escaped string was not consumed");
				require(lexer.tok == lex::token_lstr && lexer.tok.str_val == long_string, "cached lookahead was not promoted intact");
				require(lexer.next() == lex::token_lstr, "long string was not consumed");
				require(lexer.tok == lex::token_fstr, "formatted string was not scanned");
				string* formatted = lexer.tok.str_val;
				require(formatted->view() == "prefix {beta}", "formatted string contents were corrupted");

				require(lexer.next() == lex::token_fstr, "formatted string was not consumed");
				require(lexer.next() == lex::token_match, "match keyword was not preserved");
				require(lexer.next() == lex::token_leave, "leave keyword was not preserved");
				auto step = lexer.next();
				require(step == lex::token_name && step.str_val->view() == "step", "contextual step must remain an identifier");
				require(lexer.next() == lex::token_fatarrow, "fat arrow token was not preserved");
				require(lexer.tok == lex::token_name, "trailing identifier was not scanned");
				string* omega = lexer.tok.str_val;
				require(lexer.next() == lex::token_name && lexer.tok == lex::token_eof, "trailing identifier or EOF was not consumed");

				require(alpha->view() == "alpha", "identifier borrow dangled after full scan");
				require(escaped->view() == "line\nvalue", "string borrow dangled after full scan");
				require(long_string->view() == "long\nvalue", "lookahead borrow dangled after full scan");
				require(formatted->view() == "prefix {beta}", "formatted string borrow dangled after full scan");
				require(omega->view() == "omega", "trailing identifier borrow dangled after full scan");

				lexer = checkpoint;
				require(lexer.tok == lex::token_name && lexer.tok.str_val == alpha, "copy assignment did not restore the checkpoint token");
				require(lexer.lookahead() == lex::token_lstr, "restored checkpoint could not scan lookahead");

				lex::state moved = std::move(lexer);
				require(moved.tok == lex::token_name && moved.tok.str_val == alpha, "move construction lost the checkpoint token");
				lex::state reassigned(L, "discarded", "lexer-lifetime-reassigned");
				reassigned = moved;
				require(reassigned.next() == lex::token_name && reassigned.tok == lex::token_lstr, "copy assignment across arenas lost cached lookahead");
				require(reassigned.tok.str_val->view() == "line\nvalue", "copied lookahead string did not survive old-arena release");

				require(with_lookahead.tok.str_val == escaped, "snapshot current token changed after divergent scan");
				require(with_lookahead.lookahead().str_val == long_string, "snapshot lookahead token changed after divergent scan");
			}
			require_live(L, baseline, "copied lookahead destruction");
		}
	}

	void failed_scan_lifetime(vm* L) {
		static constexpr std::string_view source   = R"li(allocated "still alive" [=[unterminated)li";
		const uint64_t                    baseline = L->gc.live_objects;

		for (unsigned iteration = 0; iteration != 64; ++iteration) {
			{
				lex::state lexer(L, source, "lexer-failure");
				string*    allocated  = lexer.tok.str_val;
				lex::state checkpoint = lexer;

				require(lexer.next() == lex::token_name, "failed-scan identifier was not consumed");
				require(lexer.tok == lex::token_lstr, "failed-scan string was not scanned");
				string* literal = lexer.tok.str_val;
				require(lexer.lookahead() == lex::token_error, "unterminated long string did not fail lexing");
				lex::state failed = lexer;

				require(allocated->view() == "allocated", "identifier borrow dangled on failed scan");
				require(literal->view() == "still alive", "literal borrow dangled on failed scan");
				require(failed.next() == lex::token_lstr && failed.tok == lex::token_error, "failed lookahead was not preserved by a copied state");
				require(!failed.last_error.empty(), "failed lexer state lost its diagnostic");
				require(checkpoint.tok.str_val == allocated, "failure invalidated an earlier checkpoint");
			}
			require_live(L, baseline, "failed scan destruction");
		}
	}

	void token_source_spans(vm* L) {
		lex::state lexer(L, "one\n  two + three", "lexer-spans");
		require(lexer.tok == lex::token_name && lexer.tok.source_line == 1 && lexer.tok.source_column == 1 && lexer.tok.length == 3,
			 "first token source span was incorrect");

		lexer.next();
		require(lexer.tok == lex::token_name && lexer.tok.source_line == 2 && lexer.tok.source_column == 3 && lexer.tok.length == 3,
			 "indented token source span was incorrect");

		lexer.next();
		require(lexer.tok == '+' && lexer.tok.source_line == 2 && lexer.tok.source_column == 7 && lexer.tok.length == 1, "operator source span was incorrect");

		lexer.next();
		require(lexer.tok == lex::token_name && lexer.tok.source_line == 2 && lexer.tok.source_column == 9 && lexer.tok.length == 5,
			 "trailing token source span was incorrect");
	}

	void diagnostic_rendering(vm* L) {
		lex::state lexer(L, "first\n  bad", "diagnostic-test");
		lexer.next();
		lexer.frames.push_back({"outer frame", 1});
		lexer.frames.push_back({"inner frame", 2});
		lexer.error_at(lexer.tok, "message %d", 7);
		require(lexer.last_error.find("[diagnostic-test:2:3") != std::string::npos && lexer.last_error.find("] message 7\n  bad\n  ^~~") != std::string::npos,
			 "diagnostic did not include the token location, excerpt, and underline");
		require(lexer.last_error.find("note: inner frame") != std::string::npos, "diagnostic omitted the innermost frame");
		require(lexer.last_error.find("outer frame") == std::string::npos, "non-verbose diagnostic included an outer frame");

		L->verbose_errors = true;
		lex::state verbose(L, "bad", "diagnostic-verbose");
		L->verbose_errors = false;
		require(verbose.verbose_errors, "lexer did not copy the VM verbose-errors setting");
		verbose.frames.push_back({"outer frame", 1});
		verbose.frames.push_back({"inner frame", 1});
		verbose.error("verbose message");
		require(verbose.last_error.find("note: inner frame") != std::string::npos && verbose.last_error.find("note: outer frame") != std::string::npos,
			 "verbose diagnostic did not include every frame");

		lex::state lookahead(L, "bad\nlater", "diagnostic-lookahead");
		lookahead.lookahead();
		lookahead.error("lookahead message");
		require(lookahead.last_error.starts_with("[diagnostic-lookahead:1:1"), "lookahead moved the diagnostic off the current token");
	}

	void pack_count_tokens(vm* L) {
		lex::state lexer(L, "#Tx... # comment\nnext", "lexer-pack-count");
		require(lexer.tok == '#' && lexer.tok.source_column == 1 && lexer.tok.length == 1, "pack count prefix was not tokenized");
		lexer.next();
		require(lexer.tok == lex::token_name && lexer.tok.str_val->view() == "Tx" && lexer.tok.source_column == 2, "pack count parameter name was not preserved");
		lexer.next();
		require(lexer.tok == lex::token_dots, "pack count ellipsis was not tokenized");
		lexer.next();
		require(lexer.tok == lex::token_name && lexer.tok.str_val->view() == "next" && lexer.tok.source_line == 2, "ordinary hash comment was not skipped");
	}
}

int main() {
	li::vm* L = li::vm::create();
	if (!L)
		fail("failed to create VM");

	copied_lookahead_lifetime(L);
	failed_scan_lifetime(L);
	token_source_spans(L);
	diagnostic_rendering(L);
	pack_count_tokens(L);
	L->close();
	return 0;
}
