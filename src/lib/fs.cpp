#include <lib/fs.hpp>
#include <vm/string.hpp>
#include <lang/parser.hpp>
#include <algorithm>
#include <numeric>

namespace li::lib::fs {
	any default_import(vm* L, std::string_view name) {
		// Get the file name.
		//
		std::string file_name{name};
		if (!name.ends_with(".li") && !name.ends_with(".LI") && !name.ends_with(".Li") && !name.ends_with(".lI")) {
			file_name += ".li";
		}

		// Read the file.
		//
		auto file = read_string(file_name.c_str());
		if (!file) {
			return string::format(L, "failed reading file '%s'", file_name.c_str());
		}

		// Load the script, throw as is on failure.
		// 
		any res = load_script(L, *file, file_name, name);
		if (res.is_str())
			return res;

		// Suspend GC and invoke it.
		//
		auto psuspend = std::exchange(L->gc.suspend, true); // TODO: Not okay.
		bool ok = L->call(0, res);
		L->gc.suspend = psuspend;

		// If it threw an exception, convert to string and return.
		//
		if (!ok) {
			return L->pop_stack().to_string(L);
		}

		// Otherwise, discard the result, return the module entry.
		//
		L->pop_stack();
		auto result = L->modules->get(L, string::create(L, name));
		LI_ASSERT(result.is_tbl());
		return result;
	}
};

#if !LI_NO_STD_FS
#include <cstdio>

// To ensure compatibility across STL versions, we use CSTDIO instead.
// + The locale stuff in iostream, not very cool.
//
namespace li::lib::fs {
	std::optional<std::string> read_string(const char* path) {
		// Prepare as return type to keep NRVO.
		//
		std::optional<std::string> buffer;

		// If we fail to open the file, return nullopt.
		//
		FILE *f = fopen(path, "r");
		if (!f) {
			return buffer;
		}

		// Read file length and resize the buffer.
		//
		fseek(f, 0, SEEK_END);
		buffer.emplace().resize(std::max<intptr_t>(0, ftell(f)));
		fseek(f, 0, SEEK_SET);

		// Read the string and close the file.
		//
		fread(buffer->data(), 1, buffer->size(), f);
		fclose(f);
		return buffer;
	}
};
#endif