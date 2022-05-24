#include <lang/lexer.hpp>
#include <fstream>

using namespace lightning;

int main() {
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
		printf(" %s", token.to_string().c_str());
	}
}