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
			if (k != none) {
				printf("%s->%s [hash=%zx]\n", k.to_string().c_str(), v.to_string().c_str(), k.hash());
			}
		}
	}
};

using namespace li;
static void handle_repl_io(vm* L, std::string_view input) {
	auto fn = li::load_script(L, input, "console", true);
	if (fn.is_vfn()) {
		if (!L->scall(0, fn)) {
			printf(LI_RED "Exception: ");
			L->pop_stack().print();
			printf("\n" LI_DEF);
		} else {
			auto r = L->pop_stack();
			if (r != none) {
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
#include <ir/lifter.hpp>
#include <ir/opt.hpp>

#include <ranges>

namespace li::ir::opt {


	struct interval_info {
		std::vector<bool> live;
		uint32_t          max_live = 0;
	};

	// Fills the interval information.
	//
	static void fill_intervals(procedure* proc, interval_info* i) {
		for (auto& bb : proc->basic_blocks) {
			for (auto ins : *bb) {
				for (auto& op : ins->operands) {
					if (op->is<insn>()) {
						auto n              = op->as<insn>()->name;
						i[n].live[ins->name] = true;
						i[n].max_live        = ins->name;
					}
				}
				if (ins->vt != type::none) {
					i[ins->name].live[ins->name] = true;
				}
			}

			// Extend the intervals for block length.
			//
			for (uint32_t j = 0; j != proc->next_reg_name; j++) {
				for (auto n = bb->back()->name; n != bb->front()->name; n--) {
					i[j].live[n - 1] = i[j].live[n - 1] || i[j].live[n];
				}
			}
		}
	}

	static void alloc_regs(procedure* proc) {
		// Topologically sort the procedure and rename every register.
		//
		proc->reset_names();

		// Fixup PHIs.
		//
		for (auto& bb : proc->basic_blocks) {

			for (auto phi : std::ranges::subrange(bb->begin(), bb->end_phi())) {
			}
		}


		// Create and fill interval information.
		//
		std::vector<interval_info> i{proc->next_reg_name};
		for (uint32_t j = 0; j != proc->next_reg_name; j++) {
			i[j].live.resize(proc->next_reg_name);
		}
		fill_intervals(proc, i.data());

		// Print zhe intervals.
		//
		proc->print();

		for (uint32_t j = 0; j != proc->next_reg_name; j++) {
			printf(LI_RED "%-3u" LI_DEF "", j);
			for (uint32_t k = 0; k != proc->next_reg_name; k++) {
				if (i[j].live[k])
					printf(LI_GRN "|——");
				else if (j <= k && i[j].max_live >= k)
					printf(LI_BLU "|——");
				else
					printf(LI_DEF "|  ");
			}
			printf("|\n");
		}

		// proc->next_reg_name
	}
};


// TODO: Builtin markers for tables.

static bool ir_test(vm* L, any* args, slot_t n) {
	using namespace ir;

	auto proc = lift_bc(L, args->as_vfn());
	opt::lift_phi(proc.get());
	opt::fold_identical(proc.get());
	opt::dce(proc.get());
	opt::cfg(proc.get());

	opt::alloc_regs(proc.get());

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

	return true;
}

#if LI_ARCH_WASM
static vm* emscripten_vm = nullptr;
extern "C" {
	void __attribute__((used)) runscript(const char* str) {
		handle_repl_io(emscripten_vm, str);
	}
};
#endif

int main(int argv, const char** args) {
	platform::setup_ansi_escapes();

#if !LI_ARCH_WASM
	// Create the VM.
	//
	auto* L = vm::create();
	lib::register_std(L);
	L->globals->set(L, string::create(L, "@IR"), nfunction::create(L, &ir_test));

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
	if (fn.is_vfn()) {
		auto t0 = std::chrono::high_resolution_clock::now();
		if (!L->scall(0, fn)) {
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