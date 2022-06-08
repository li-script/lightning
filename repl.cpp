#include <algorithm>
#include <fstream>
#include <iostream>
#include <lang/lexer.hpp>
#include <lang/operator.hpp>
#include <lang/parser.hpp>
#include <lib/std.hpp>
#include <optional>
#include <thread>
#include <tuple>
#include <util/llist.hpp>
#include <vector>
#include <vm/bc.hpp>
#include <vm/function.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <vm/userdata.hpp>

namespace li::debug {
	static void dump_table(table* t) {
		for (auto& [k, v] : *t) {
			if (k != nil) {
				printf("%s->%s [hash=%zx]\n", k.to_string().c_str(), v.to_string().c_str(), k.hash());
			}
		}
	}
};

using namespace li;
static void handle_repl_io(vm* L, std::string_view input) {
	auto fn = li::load_script(L, input, "console", true);
	if (fn.is_fn()) {
		if (!L->call(0, fn)) {
			printf(LI_RED "Exception: ");
			L->pop_stack().print();
			printf("\n" LI_DEF);
		} else {
			auto r = L->pop_stack();
			if (r != nil) {
				printf(LI_GRN "");
				r.print();
				printf("\n" LI_DEF);
				if (r.is_tbl())
					debug::dump_table(r.as_tbl());
			}
		}
	} else {
		printf(LI_RED "Parser error: " LI_DEF);
		fn.print();
		putchar('\n');
	}
}



#include <list>
#include <memory>
#include <vm/bc.hpp>
#include <lang/types.hpp>
#include <vm/gc.hpp>

#include <ir/insn.hpp>
#include <ir/bc2ir.hpp>
#include <ir/ir2mir.hpp>
#include <ir/opt.hpp>
#include <ir/mir.hpp>

#if LI_JIT
namespace li::jit {
	using namespace ir;

	static bool on(vm* L, any* args, slot_t n) {
		if (!args->is_fn() || args->as_fn()->is_native()) {
			return L->error("expected vfunction.");
		}

		if (!args->as_fn()->proto->jfunc) {

			bool verbose = n > 1 && args[-1].coerce_bool();

			auto proc = lift_bc(L, args->as_fn()->proto);
			opt::lift_phi(proc.get());

			opt::fold_constant(proc.get());
			opt::fold_identical(proc.get());
			opt::dce(proc.get());
			opt::cfg(proc.get());

			opt::type_split_cfg(proc.get());
			opt::type_inference(proc.get());
			opt::fold_constant(proc.get());
			opt::fold_identical(proc.get());
			opt::dce(proc.get());
			opt::cfg(proc.get());

			// empty pass
			opt::type_inference(proc.get());
			opt::fold_constant(proc.get());
			opt::fold_identical(proc.get());
			opt::dce(proc.get());
			opt::cfg(proc.get());

			opt::prepare_for_mir(proc.get());
			opt::type_inference(proc.get());
			opt::fold_constant(proc.get());
			opt::fold_identical(proc.get());
			opt::dce(proc.get());
			opt::cfg(proc.get());

			opt::finalize_for_mir(proc.get());
			if (verbose)
				proc->print();

			auto mp = lift_ir(proc.get());

			opt::remove_redundant_setcc(mp.get());
			opt::allocate_registers(mp.get());
			if (verbose)
				mp->print();

			args->as_fn()->proto->jfunc = assemble_ir(mp.get());
			//
			//

			// hoist table fields even if it escapes
			// move stuff out of loops
			// type inference
			// trait inference
			// constant folding
			// escape analysis
			// loop analysis
			// handling of frozen tables + add builtin tables
		}

		args->as_fn()->invoke = (nfunc_t) &args->as_fn()->proto->jfunc->code[0];
		return L->ok();
	}
	static bool off(vm* L, any* args, slot_t n) {
		if (!args->is_fn() || args->as_fn()->is_native()) {
			return L->error("expected vfunction.");
		}
		args->as_fn()->invoke = &vm_invoke;
		return L->ok();
	}
	static bool bp(vm* L, any* args, slot_t n) {
		if (!args->is_fn() || args->as_fn()->is_native() || !args->as_fn()->is_jit()) {
			return L->error("expected vfunction with JIT record.");
		}
		args->as_fn()->proto->jfunc->code[0] = 0xCC;
		return L->ok();
	}
	static bool where(vm* L, any* args, slot_t n) {
		if (!args->is_fn() || args->as_fn()->is_native() || !args->as_fn()->is_jit()) {
			return L->ok("N/A");
		}
		return L->ok(string::format(L, "%p", &args->as_fn()->proto->jfunc->code[0]));
	}
	static bool disasm(vm* L, any* args, slot_t n) {
		if (!args->is_fn() || args->as_fn()->is_native() || !args->as_fn()->is_jit()) {
			return L->error("expected vfunction with JIT record.");
		}

		auto*       jf     = args->as_fn()->proto->jfunc;

		std::string result = {};
		auto gen = std::span<const uint8_t>(jf->code, jf->object_bytes());
		while (auto i = zy::decode(gen)) {
			if (i->ins.mnemonic == ZYDIS_MNEMONIC_INT3)
				break;
			result += i->to_string();
			result += '\n';
		}
		return L->ok(string::create(L, result));
	}
};
#endif

#if LI_ARCH_WASM
static vm* emscripten_vm = nullptr;
extern "C" {
	void __attribute__((used)) runscript(const char* str) {
		handle_repl_io(emscripten_vm, str);
	}
};
#endif

#include <util/user.hpp>
int main(int argv, const char** args) {
	platform::setup_ansi_escapes();

#if !LI_ARCH_WASM
	// Create the VM.
	//
	auto* L = vm::create();
	lib::register_std(L);
	util::export_as(L, "jit.on", jit::on);
	util::export_as(L, "jit.off", jit::off);
	util::export_as(L, "jit.bp", jit::bp);
	util::export_as(L, "jit.where", jit::where);
	util::export_as(L, "jit.disasm", jit::disasm);

	// Repl if no file given.
	//
	if (argv < 2) {
		// clang-format off
		static constexpr const char hdr[] =
		LI_YLW "                 @          " LI_CYN "                                          \n"
		LI_YLW "               @@           " LI_CYN "                                          \n"
		LI_YLW "            ,@@@            " LI_CYN "   _      _  _____           _       _    \n" 
		LI_YLW "          @@@@@             " LI_CYN "  | |    (_)/ ____|         (_)     | |   \n" 
		LI_YLW "       ,@@@@@@              " LI_CYN "  | |     _| (___   ___ _ __ _ _ __ | |_  \n" 
		LI_YLW "     @@@@@@@@               " LI_CYN "  | |    | |\\___ \\ / __| '__| | '_ \\| __| \n" 
		LI_YLW "  ,@@@@@@@@@@@@@@@@@@@@@@@  " LI_CYN "  | |____| |____) | (__| |  | | |_) | |_  \n" 
		LI_YLW "               @@@@@@@@,    " LI_CYN "  |______|_|_____/ \\___|_|  |_| .__/ \\__| \n" 
		LI_YLW "              @@@@@@@       " LI_CYN "                              | |         \n" 
		LI_YLW "             @@@@@,         " LI_CYN "                              |_|         \n"
		LI_YLW "             @@@            " LI_CYN "                                          \n"
		LI_YLW "            @,              " LI_CYN "                                          \n" LI_DEF;
		// clang-format on
		puts(hdr);

		while (true) {
			std::string buffer;
			fputs(LI_BRG "> " LI_DEF, stdout);
			std::getline(std::cin, buffer);

			// While shift is being held, allow multiple lines to be inputted.
			//
			while (platform::is_shift_down()) {
				std::string buffer2;
				fputs("  ", stdout);
				std::getline(std::cin, buffer2);
				buffer += "\n";
				buffer += buffer2;
			}

			// Exit on EOF (CTRL+D).
			//
			if (std::cin.eof()) {
				return 0;
			}

			// Execute and print.
			//
			handle_repl_io(L, buffer);
		}
	}

	// Read the file.
	//
	std::ifstream file(args[1]);
	if (!file.good()) {
		printf(LI_RED "Failed reading file '%s'\n" LI_DEF, args[1]);
		return 1;
	}
	std::string file_buf{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	auto        fn = li::load_script(L, file_buf, args[1]);

	// Validate, print the result.
	//
	int retval = 1;
	if (fn.is_fn()) {
		auto t0 = std::chrono::high_resolution_clock::now();
		if (!L->call(0, fn)) {
			auto t1 = std::chrono::high_resolution_clock::now();
			printf(LI_BLU "(%.2lf ms) " LI_RED "Exception: " LI_DEF, (t1 - t0) / std::chrono::duration<double, std::milli>(1.0));
			L->pop_stack().print();
			putchar('\n');
		} else {
			auto t1 = std::chrono::high_resolution_clock::now();
			printf(LI_BLU "(%.2lf ms) " LI_GRN "Result: " LI_DEF, (t1 - t0) / std::chrono::duration<double, std::milli>(1.0));
			auto r = L->pop_stack();
			r.print();
			putchar('\n');
			if (r.is_tbl())
				debug::dump_table(r.as_tbl());
			retval = 0;
		}
	} else {
		printf(LI_RED "Parser error: " LI_DEF);
		fn.print();
		putchar('\n');
	}
	L->close();
	return retval;
#else
	emscripten_vm = vm::create();
	lib::register_std(emscripten_vm);
	return 0;
#endif
}