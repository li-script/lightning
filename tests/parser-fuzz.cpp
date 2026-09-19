#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <lang/parser.hpp>
#include <vm/rc.hpp>
#include <vm/state.hpp>

namespace {
	using byte = std::uint8_t;

	constexpr std::size_t default_iterations  = 2000;
	constexpr std::size_t default_max_input   = 64 * 1024;
	constexpr std::size_t default_max_depth   = 64;
	constexpr std::size_t hard_max_input      = 1024 * 1024;
	constexpr std::size_t hard_max_depth      = 256;
	constexpr std::size_t hard_max_iterations = 100000;
	constexpr std::size_t max_corpus_file     = 1024 * 1024;

	struct options {
		std::uint64_t seed       = UINT64_C(0x6c696768746e696e);
		std::size_t   iterations = default_iterations;
		std::size_t   max_input  = default_max_input;
		std::size_t   max_depth  = default_max_depth;
		std::size_t   only_case  = std::numeric_limits<std::size_t>::max();
		std::string   corpus_path;
	};

	struct corpus_case {
		std::string       name;
		std::vector<byte> bytes;
	};

	class splitmix64 {
		std::uint64_t state;

	  public:
		explicit splitmix64(std::uint64_t seed) : state(seed) {}

		std::uint64_t next() {
			std::uint64_t value = (state += UINT64_C(0x9e3779b97f4a7c15));
			value               = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
			value               = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
			return value ^ (value >> 31);
		}

		std::size_t bounded(std::size_t limit) { return limit ? static_cast<std::size_t>(next() % limit) : 0; }
	};

	void usage(const char* executable) {
		std::fprintf(stderr, "usage: %s [--seed N] [--iterations N] [--case N] [--max-input-size N] [--max-depth N] [--corpus PATH]\n", executable);
	}

	bool parse_integer(std::string_view text, std::uint64_t& value) {
		int base = 10;
		if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
			text.remove_prefix(2);
			base = 16;
		}
		if (text.empty())
			return false;
		auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
		return result.ec == std::errc{} && result.ptr == text.data() + text.size();
	}

	bool take_option_value(int& index, int argc, char** argv, std::string_view argument, std::string_view option, std::string_view& value) {
		if (argument == option) {
			if (++index == argc)
				return false;
			value = argv[index];
			return true;
		}
		if (argument.size() > option.size() && argument.starts_with(option) && argument[option.size()] == '=') {
			value = argument.substr(option.size() + 1);
			return true;
		}
		return false;
	}

	bool parse_options(int argc, char** argv, options& result, bool& show_help) {
		show_help = false;
		for (int index = 1; index != argc; ++index) {
			std::string_view argument = argv[index];
			std::string_view value;
			std::uint64_t    number = 0;
			if (argument == "--help" || argument == "-h") {
				show_help = true;
				return true;
			}
			if (take_option_value(index, argc, argv, argument, "--seed", value)) {
				if (!parse_integer(value, result.seed))
					return false;
				continue;
			}
			if (take_option_value(index, argc, argv, argument, "--iterations", value)) {
				if (!parse_integer(value, number) || number > hard_max_iterations)
					return false;
				result.iterations = static_cast<std::size_t>(number);
				continue;
			}
			if (take_option_value(index, argc, argv, argument, "--case", value)) {
				if (!parse_integer(value, number) || number > std::numeric_limits<std::size_t>::max())
					return false;
				result.only_case = static_cast<std::size_t>(number);
				continue;
			}
			if (take_option_value(index, argc, argv, argument, "--max-input-size", value)) {
				if (!parse_integer(value, number) || number == 0 || number > hard_max_input)
					return false;
				result.max_input = static_cast<std::size_t>(number);
				continue;
			}
			if (take_option_value(index, argc, argv, argument, "--max-depth", value)) {
				if (!parse_integer(value, number) || number == 0 || number > hard_max_depth)
					return false;
				result.max_depth = static_cast<std::size_t>(number);
				continue;
			}
			if (take_option_value(index, argc, argv, argument, "--corpus", value)) {
				if (value.empty())
					return false;
				result.corpus_path.assign(value);
				continue;
			}
			return false;
		}
		return result.only_case == std::numeric_limits<std::size_t>::max() || result.only_case < result.iterations;
	}

	int hex_digit(char value) {
		if (value >= '0' && value <= '9')
			return value - '0';
		if (value >= 'a' && value <= 'f')
			return value - 'a' + 10;
		if (value >= 'A' && value <= 'F')
			return value - 'A' + 10;
		return -1;
	}

	bool decode_corpus_bytes(std::string_view encoded, std::size_t maximum, std::vector<byte>& result, std::string& error) {
		result.clear();
		result.reserve(std::min(encoded.size(), maximum));
		for (std::size_t index = 0; index != encoded.size();) {
			if (result.size() == maximum) {
				error = "decoded input exceeds --max-input-size";
				return false;
			}
			char current = encoded[index++];
			if (current != '\\') {
				result.push_back(static_cast<byte>(static_cast<unsigned char>(current)));
				continue;
			}
			if (index == encoded.size()) {
				error = "dangling corpus escape";
				return false;
			}
			char escaped = encoded[index++];
			switch (escaped) {
				case '\\':
					result.push_back('\\');
					break;
				case 'n':
					result.push_back('\n');
					break;
				case 'r':
					result.push_back('\r');
					break;
				case 't':
					result.push_back('\t');
					break;
				case '0':
					result.push_back(0);
					break;
				case 'x': {
					if (encoded.size() - index < 2) {
						error = "short hexadecimal corpus escape";
						return false;
					}
					int high = hex_digit(encoded[index]);
					int low  = hex_digit(encoded[index + 1]);
					if (high < 0 || low < 0) {
						error = "invalid hexadecimal corpus escape";
						return false;
					}
					result.push_back(static_cast<byte>((high << 4) | low));
					index += 2;
					break;
				}
				default:
					error = "unknown corpus escape";
					return false;
			}
		}
		return true;
	}

	bool identifier_byte(byte value) {
		return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == '_';
	}

	bool contains_import_keyword(const std::vector<byte>& input, std::size_t* position = nullptr) {
		static constexpr std::string_view keyword = "import";
		if (input.size() < keyword.size())
			return false;
		for (std::size_t index = 0; index + keyword.size() <= input.size(); ++index) {
			bool equal = true;
			for (std::size_t offset = 0; offset != keyword.size(); ++offset)
				equal &= input[index + offset] == static_cast<byte>(keyword[offset]);
			if (!equal)
				continue;
			bool left_boundary  = index == 0 || !identifier_byte(input[index - 1]);
			bool right_boundary = index + keyword.size() == input.size() || !identifier_byte(input[index + keyword.size()]);
			if (left_boundary && right_boundary) {
				if (position)
					*position = index;
				return true;
			}
		}
		return false;
	}

	bool load_corpus(const std::string& path, std::size_t maximum, std::size_t maximum_depth, std::vector<corpus_case>& corpus, std::string& error) {
		std::ifstream stream(path, std::ios::binary);
		if (!stream) {
			error = "cannot open parser corpus '" + path + "'";
			return false;
		}
		stream.seekg(0, std::ios::end);
		std::streamoff file_size = stream.tellg();
		if (file_size < 0 || static_cast<std::uint64_t>(file_size) > max_corpus_file) {
			error = "parser corpus exceeds the 1 MiB file limit";
			return false;
		}
		stream.seekg(0, std::ios::beg);

		std::string line;
		std::size_t line_number = 0;
		while (std::getline(stream, line)) {
			++line_number;
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line.empty() || line[0] == '#')
				continue;
			std::size_t separator = line.find('|');
			if (separator == std::string::npos || separator == 0) {
				error = "invalid parser corpus record on line " + std::to_string(line_number);
				return false;
			}
			corpus_case entry;
			entry.name = line.substr(0, separator);
			if (!decode_corpus_bytes(std::string_view(line).substr(separator + 1), maximum, entry.bytes, error)) {
				error = "parser corpus line " + std::to_string(line_number) + ": " + error;
				return false;
			}
			if (contains_import_keyword(entry.bytes)) {
				error = "parser corpus line " + std::to_string(line_number) + " contains an import and would mutate VM module state";
				return false;
			}
			std::size_t depth = 0;
			for (byte value : entry.bytes) {
				if (value == '(' || value == '[' || value == '{') {
					if (++depth > maximum_depth) {
						error = "parser corpus line " + std::to_string(line_number) + " exceeds --max-depth";
						return false;
					}
				} else if ((value == ')' || value == ']' || value == '}') && depth) {
					--depth;
				}
			}
			corpus.emplace_back(std::move(entry));
		}
		if (!stream.eof()) {
			error = "failed reading parser corpus '" + path + "'";
			return false;
		}
		if (corpus.empty()) {
			error = "parser corpus is empty";
			return false;
		}
		return true;
	}

	std::vector<byte> bytes_of(std::string_view text) { return {text.begin(), text.end()}; }

	void append_ascii(std::vector<byte>& output, std::string_view text, std::size_t maximum) {
		std::size_t count = std::min(text.size(), maximum - output.size());
		output.insert(output.end(), text.begin(), text.begin() + static_cast<std::ptrdiff_t>(count));
	}

	void mutate_input(std::vector<byte>& input, splitmix64& random, std::size_t maximum) {
		static constexpr byte alphabet[] = {
			 0x00,
			 0x09,
			 0x0a,
			 0x0d,
			 0x20,
			 '#',
			 '"',
			 '\'',
			 '`',
			 '\\',
			 '(',
			 ')',
			 '[',
			 ']',
			 '{',
			 '}',
			 ',',
			 '.',
			 ':',
			 ';',
			 '?',
			 '=',
			 '+',
			 '-',
			 '*',
			 '/',
			 '%',
			 '&',
			 '|',
			 '!',
			 '<',
			 '>',
			 'a',
			 'b',
			 'c',
			 'f',
			 'n',
			 'x',
			 '0',
			 '1',
			 '9',
			 0x7f,
			 0x80,
			 0xbf,
			 0xc0,
			 0xc3,
			 0xef,
			 0xf0,
			 0xff,
		};
		std::size_t operations = 1 + random.bounded(8);
		for (std::size_t operation = 0; operation != operations; ++operation) {
			switch (random.bounded(6)) {
				case 0:
					if (!input.empty())
						input[random.bounded(input.size())] ^= static_cast<byte>(1u << random.bounded(8));
					break;
				case 1:
					if (!input.empty())
						input.erase(input.begin() + static_cast<std::ptrdiff_t>(random.bounded(input.size())));
					break;
				case 2:
					if (input.size() < maximum) {
						std::size_t position = random.bounded(input.size() + 1);
						input.insert(input.begin() + static_cast<std::ptrdiff_t>(position), alphabet[random.bounded(std::size(alphabet))]);
					}
					break;
				case 3:
					if (!input.empty())
						input.resize(random.bounded(input.size() + 1));
					break;
				case 4:
					if (!input.empty() && input.size() < maximum) {
						std::size_t begin     = random.bounded(input.size());
						std::size_t available = std::min<std::size_t>(32, input.size() - begin);
						std::size_t count     = 1 + random.bounded(available);
						count                 = std::min(count, maximum - input.size());
						std::vector<byte> copy(input.begin() + static_cast<std::ptrdiff_t>(begin), input.begin() + static_cast<std::ptrdiff_t>(begin + count));
						std::size_t       position = random.bounded(input.size() + 1);
						input.insert(input.begin() + static_cast<std::ptrdiff_t>(position), copy.begin(), copy.end());
					}
					break;
				default:
					if (!input.empty())
						input[random.bounded(input.size())] = alphabet[random.bounded(std::size(alphabet))];
					break;
			}
		}
	}

	std::vector<byte> encoded_bom_case(splitmix64& random) {
		static constexpr std::string_view source = "const value = 42\nvalue";
		std::vector<byte>                 output;
		switch (random.bounded(5)) {
			case 0:
				output = {0xef, 0xbb, 0xbf};
				append_ascii(output, source, hard_max_input);
				break;
			case 1:
				output = {0xff, 0xfe};
				for (unsigned char value : source) {
					output.push_back(value);
					output.push_back(0);
				}
				break;
			case 2:
				output = {0xfe, 0xff};
				for (unsigned char value : source) {
					output.push_back(0);
					output.push_back(value);
				}
				break;
			case 3:
				output = {0xff, 0xfe, 0x00, 0x00};
				for (unsigned char value : source)
					output.insert(output.end(), {value, 0, 0, 0});
				break;
			default:
				output = {0x00, 0x00, 0xfe, 0xff};
				for (unsigned char value : source)
					output.insert(output.end(), {0, 0, 0, value});
				break;
		}
		if (random.bounded(3) == 0 && !output.empty())
			output.resize(output.size() - 1 - random.bounded(std::min<std::size_t>(3, output.size())));
		return output;
	}

	std::vector<byte> make_generated_case(std::size_t iteration, splitmix64& random, const std::vector<corpus_case>& corpus, const options& settings) {
		std::vector<byte> output;
		switch (iteration % 9) {
			case 0:
				output = corpus[random.bounded(corpus.size())].bytes;
				mutate_input(output, random, settings.max_input);
				break;
			case 1:
				output = encoded_bom_case(random);
				break;
			case 2: {
				static constexpr std::string_view escapes[] = {
					 "const value = \"truncated\\",
					 "const value = \"hex\\x\"",
					 "const value = \"unicode\\u{110000}\"",
					 "const value = `format {1 + }`",
					 "const value = '\\q'",
				};
				output = bytes_of(escapes[random.bounded(std::size(escapes))]);
				break;
			}
			case 3: {
				std::size_t equals = random.bounded(5);
				std::string delimiter(equals, '=');
				std::string source = "const value = [" + delimiter + "[long ]=] text\nsecond line]";
				if (random.bounded(2))
					source += delimiter;
				source += "]";
				output = bytes_of(source);
				break;
			}
			case 4: {
				static constexpr std::string_view comments[] = {
					 "# line comment without final newline",
					 "#[=[ long comment ]=]\nconst value = 1",
					 "#[==[ unterminated long comment ]=]",
					 "#[====[ nested-looking [=[ text ]=] ]====]\n42",
				};
				output = bytes_of(comments[random.bounded(std::size(comments))]);
				break;
			}
			case 5: {
				std::size_t depth = 1 + random.bounded(settings.max_depth);
				output.reserve(depth * 2 + 1);
				output.insert(output.end(), depth, '(');
				output.push_back('1');
				output.insert(output.end(), depth - random.bounded(2), ')');
				break;
			}
			case 6: {
				std::string suffix = std::to_string(random.bounded(10000));
				std::string source = "fn fuzz" + suffix +
											"(value: number) {\n"
											"  defer { const captured = [value, {nested: value}] }\n"
											"  value + 1\n}\n"
											"class Box" +
											suffix +
											" { value: number\nread() { self.value } }\n"
											"fuzz" +
											suffix + "(41)";
				output             = bytes_of(source);
				break;
			}
			case 7: {
				static constexpr std::string_view operators[] = {
					 "let value = nil\nvalue ??= 1\nvalue += 2\nvalue * 3 <= 9 && true",
					 "const value = 1..=3\nvalue",
					 "let value = {field: [1, 2]}\nvalue.field[0]++",
					 "true ? 1 : false ? 2 : 3",
				};
				output = bytes_of(operators[random.bounded(std::size(operators))]);
				break;
			}
			default: {
				std::size_t length = 1 + random.bounded(std::min<std::size_t>(settings.max_input, 1024));
				output.resize(length);
				for (byte& value : output)
					value = static_cast<byte>(random.next());
				break;
			}
		}

		if (output.size() > settings.max_input)
			output.resize(settings.max_input);
		std::size_t import_position = 0;
		while (contains_import_keyword(output, &import_position))
			output[import_position] = '_';

		std::size_t depth = 0;
		for (byte& value : output) {
			if (value == '(' || value == '[' || value == '{') {
				if (depth == settings.max_depth)
					value = ' ';
				else
					++depth;
			} else if ((value == ')' || value == ']' || value == '}') && depth) {
				--depth;
			}
		}
		return output;
	}

	bool run_parser_case(li::vm* state, const std::vector<byte>& input, std::string& failure) {
		const std::uint64_t baseline = state->gc.live_objects;
		li::any             loaded   = li::nil;
		bool                threw    = false;
		try {
			const char* data = input.empty() ? "" : reinterpret_cast<const char*>(input.data());
			loaded           = li::load_script(state, std::string_view(data, input.size()), "parser-fuzz", "", false);
		} catch (const std::exception& error) {
			failure = std::string("load_script threw: ") + error.what();
			threw   = true;
		} catch (...) {
			failure = "load_script threw a non-standard exception";
			threw   = true;
		}

		bool syntax_error    = loaded.is_exc();
		bool function_result = loaded.is_fn();
		bool error_owned     = state->last_ex.is_str();
		bool stray_error     = !syntax_error && state->last_ex != li::nil;
		li::rc::release(state, loaded);
		state->clear_exception();

		if (!threw && syntax_error && !error_owned)
			failure = "syntax failure did not retain a string diagnostic";
		else if (!threw && !syntax_error && !function_result)
			failure = "load_script returned neither a function nor an exception";
		else if (!threw && stray_error)
			failure = "successful compilation left a pending error";
		if (state->gc.live_objects != baseline) {
			failure = "compiler ownership imbalance: expected " + std::to_string(baseline) + " live objects, observed " + std::to_string(state->gc.live_objects);
		}
		return failure.empty();
	}

	void print_failure(const options& settings, std::string_view case_name, const std::vector<byte>& input, std::string_view reason) {
		std::fprintf(stderr, "parser-fuzz failure: %.*s\nseed=0x%016llx size=%zu reason=%.*s\nbytes=", static_cast<int>(case_name.size()), case_name.data(),
			 static_cast<unsigned long long>(settings.seed), input.size(), static_cast<int>(reason.size()), reason.data());
		for (byte value : input)
			std::fprintf(stderr, "%02x", static_cast<unsigned>(value));
		std::fputc('\n', stderr);
	}
}

int main(int argc, char** argv) {
	options settings;
	bool    show_help = false;
	if (!parse_options(argc, argv, settings, show_help)) {
		usage(argv[0]);
		return 2;
	}
	if (show_help) {
		usage(argv[0]);
		return 0;
	}

	if (settings.corpus_path.empty()) {
		std::ifstream rooted("tests/parser-corpus.txt", std::ios::binary);
		settings.corpus_path = rooted ? "tests/parser-corpus.txt" : "parser-corpus.txt";
	}
	std::vector<corpus_case> corpus;
	std::string              error;
	if (!load_corpus(settings.corpus_path, settings.max_input, settings.max_depth, corpus, error)) {
		std::fprintf(stderr, "parser-fuzz: %s\n", error.c_str());
		return 2;
	}

	li::vm* state = li::vm::create();
	if (!state) {
		std::fprintf(stderr, "parser-fuzz: failed to create VM\n");
		return 1;
	}
	std::fprintf(stderr, "parser-fuzz: seed=0x%016llx iterations=%zu max-input=%zu max-depth=%zu\n", static_cast<unsigned long long>(settings.seed),
		 settings.iterations, settings.max_input, settings.max_depth);

	if (settings.only_case == std::numeric_limits<std::size_t>::max()) {
		for (const corpus_case& entry : corpus) {
			error.clear();
			if (!run_parser_case(state, entry.bytes, error)) {
				print_failure(settings, std::string("corpus/") + entry.name, entry.bytes, error);
				state->close();
				return 1;
			}
		}
	}

	splitmix64 random(settings.seed);
	for (std::size_t iteration = 0; iteration != settings.iterations; ++iteration) {
		std::vector<byte> input = make_generated_case(iteration, random, corpus, settings);
		if (settings.only_case != std::numeric_limits<std::size_t>::max() && iteration != settings.only_case)
			continue;
		error.clear();
		if (!run_parser_case(state, input, error)) {
			print_failure(settings, std::string("generated/") + std::to_string(iteration), input, error);
			state->close();
			return 1;
		}
	}

	state->close();
	return 0;
}
