#include <lang/lexer.hpp>
#include <fstream>





namespace lightning::parser {

	enum class expression {

	};



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
	std::string strx = {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};

	lightning::lexer::state lexer{std::move(strx)};
	
	size_t last_line = 0;
	while (true) {
		auto& token = lexer.next();
		if (token.id == lexer::token_eof)
			break;
		if (last_line != lexer.line) {
			printf("\n%03llu: ", lexer.line);
			last_line = lexer.line;
		}
		putchar(' ');
		token.print();
	}
}