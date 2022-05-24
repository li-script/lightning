#include <lang/lexer.hpp>
#include <fstream>





namespace lightning::parser {

	enum class expression {

	};



	static void parse() {

	}

};




using namespace lightning;

#ifdef _WIN32
	#include <Windows.h>
#endif

int main() {
	#ifdef _WIN32
		#define WIN32_LEAN_AND_MEAN
		auto console_handle = GetStdHandle(STD_OUTPUT_HANDLE);
		SetConsoleOutputCP(CP_UTF8);
		DWORD mode = 0;
		GetConsoleMode(console_handle, &mode);
		mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
		SetConsoleMode(console_handle, mode);
	#endif

	std::ifstream file("S:\\Projects\\Lightning\\lexer-test.li");
	lightning::lexer::state lexer{std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}};
	
	size_t last_line = 0;
	while (true) {
		if (last_line != lexer.line) {
			printf("\n%03llu: ", lexer.line);
			last_line = lexer.line;
		}
		auto token = lexer.next();
		if (token == lexer::token_eof)
			break;
		putchar(' ');
		token.print();
	}
}