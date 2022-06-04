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
#include <jit/regalloc.hpp>
#include <jit/target.hpp>

// TODO: Builtin markers for tables.
// math.sqrt -> sqrtss
// type / trait stuff

static bool ir_test(vm* L, any* args, slot_t n) {
	using namespace ir;

	auto proc = lift_bc(L, args->as_vfn());
	opt::lift_phi(proc.get());
	opt::fold_identical(proc.get());
	opt::dce(proc.get());
	opt::cfg(proc.get());

	auto get_unreachable_block = [&proc, unreachable_block = (basic_block*) nullptr]() mutable {
		if (unreachable_block)
			return unreachable_block;
		auto* b = proc->add_block();
		builder{b}.emit_front<unreachable>();
		b->cold_hint = 100;
		unreachable_block = b;
		return b;
	};

	proc->bfs([&](basic_block* bb) {
		// Find the first numeric operation with an unknown type.
		//
		auto i = range::find_if(bb->insns(), [](insn* i) {
			if (i->is<binop>() || i->is<compare>()) {
				i->update();
				if (i->vt != type::unk) {
					for (auto& op : i->operands)
						op->update();
				} else {
					return true;
				}
			}
			return false;
		});
		if (i == bb->end()) {
			return false;
		}

		// Insert an is number check before.
		//
		builder b{i.at};
		auto op = i->operands[1]->vt == type::unk ? 1 : 2; 
		auto cc = b.emit_before<test_type>(i.at, i->operands[op], type_number);

		// Split the block after the check, add the jcc.
		//
		auto cont_blk = bb->split_at(cc);
		auto num_blk    = proc->add_block();
		auto nonnum_blk = get_unreachable_block();  // proc->add_block();
		b.emit<jcc>(cc, num_blk, nonnum_blk);
		proc->add_jump(bb, num_blk);
		proc->add_jump(bb, nonnum_blk);

		// Unlink the actual operation and move it to another block.
		//
		b.blk           = num_blk;
		i->operands[op] = b.emit<assume_cast>(i->operands[op], type::f64);
		auto v1         = num_blk->push_back(i->erase());
		v1->update();
		b.emit<jmp>(cont_blk);
		proc->add_jump(num_blk, cont_blk);

		// Replace uses.
		//
		i->for_each_user_outside_block([&](insn* i, size_t op) {
			i->operands[op].reset(v1.at);
			return false;
		});

		// TODO: Trait stuff.
		//b.blk           = nonnum_blk;
		//auto v2 = i->is<compare>() ? b.emit<move>(false) : b.emit<move>(0);
		//b.emit<jmp>(cont_blk);
		//proc->add_jump(nonnum_blk, cont_blk);
		//
		//// Create a PHI at the destination.
		////
		//b.blk = cont_blk;
		//auto x = b.emit_front<phi>(v1.at, v2.at);
		//
		//// Replace uses.
		////
		//cont_blk->validate();
		//i->for_each_user_outside_block([&](insn* i, size_t op) {
		//	if (i!=x)
		//		i->operands[op].reset(x);
		//	i->parent->validate();
		//	return false;
		//});
		return false;
	});
	opt::fold_identical(proc.get());
	opt::dce(proc.get());
	opt::cfg(proc.get());

	proc->topological_sort();

	proc->validate();
	proc->print();

	nfunction* res = 0;
	auto err = jit::generate_code(proc.get(), &res);
	if (err) {
		printf("error during jit codegen: %s\n", err->c_str());
	} else {
		L->push_stack(res);
	}

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