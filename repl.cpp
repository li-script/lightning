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


namespace li::ir::opt {


	struct register_state {
		// Live interval and cached min-max.
		//
		std::vector<bool> live;
		uint32_t          max_live = 0;
		uint32_t          min_live = 0;

		// Current register and spill slot.
		//
		arch::reg phys_reg = 0;
		int32_t   spill_slot = -1;
	};

	struct reg_allocator {
		procedure*                  proc;
		std::vector<register_state> vreg;
		uint64_t                    allocated_gp_regs = 0;
		uint64_t                    allocated_fp_regs = 0;
		std::vector<int32_t>        spill_stack       = {};

		// Initializes a state to be filled by linear allocator.
		//
		reg_allocator(procedure* proc) : proc(proc) {
			// Initiliaze the register array.
			//
			uint32_t n = proc->next_reg_name;
			vreg.resize(n);
			for (uint32_t j = 0; j != n; j++) {
				vreg[j].live.resize(n);
			}
		}

		// Merges two names.
		//
		void merge(uint32_t dst, uint32_t src) {
			for (size_t n = 0; n != vreg.size(); n++) {
				vreg[src].live[n] = vreg[dst].live[n] || vreg[src].live[n];
			}
			vreg[src].max_live = std::max(vreg[src].max_live, vreg[dst].max_live);
			vreg[src].min_live = std::min(vreg[src].min_live, vreg[dst].min_live);
			vreg[dst].min_live = 0;
			vreg[dst].max_live = 0;
			vreg[dst].live.clear();
			vreg[dst].live.resize(vreg.size(), false);
		}

		// Prints interval graph.
		//
		void print_intervals() const {
			printf("   ");
			for (uint32_t j = 0; j != vreg.size(); j++) {
				printf("|%02x",j);
			}
			printf("|\n");
			for (uint32_t j = 0; j != vreg.size(); j++) {
				if (vreg[j].max_live == 0)
					continue;
				printf(LI_RED "%-3u" LI_DEF "", j);
				for (uint32_t k = 0; k != vreg.size(); k++) {
					if (vreg[j].live[k])
						printf(LI_BLU "|++");
					else if (vreg[j].min_live <= k && k <= vreg[j].max_live)
						printf(LI_DEF "|——");
					else
						printf(LI_DEF "|  ");
				}
				printf("|\n" LI_DEF);
			}
		}

		// Gets the currently associated reg with the name if there is one.
		//
		arch::reg check_reg(uint32_t name) const { return vreg[name].phys_reg; }

		// TODO: Will be callback to codegen context.
		//
		void store(arch::reg r, uint32_t slot) {
			printf("store(%s -> stack[%u])\n", arch::name_native(arch::to_native(r)), slot);
		}
		void load(arch::reg r, uint32_t slot) {
			printf("load(%s <- stack[%u])\n", arch::name_native(arch::to_native(r)), slot);
		}
		void move(arch::reg dst, arch::reg src) {
			printf("move(%s <- %s)\n", arch::name_native(arch::to_native(dst)), arch::name_native(arch::to_native(src)));
		}

		// Marks a register allocated/freed.
		//
		void mark_alloc(arch::reg r) {
			if (r > 0)
				allocated_gp_regs |= 1ull << (r - 1);
			else if (r < 0)
				allocated_fp_regs |= 1ull << (-(r - 1));
			else
				util::abort("allocating null register.");
		}
		void mark_free(arch::reg r) {
			if (r > 0)
				allocated_gp_regs &= ~(1ull << (r - 1));
			else if (r < 0)
				allocated_fp_regs &= ~(1ull << (-(r - 1)));
		}
		bool is_free(arch::reg r) {
			if (r > 0)
				return allocated_gp_regs & (1ull << (r - 1));
			else if (r < 0)
				return allocated_fp_regs & (1ull << (-(r - 1)));
			else
				util::abort("testing null register.");
		}

		// Spills a register.
		//
		void spill(uint32_t ip, uint32_t owner, bool noreg = false) {
			// If register is dead after this, ignore.
			//
			auto& r = vreg[owner];
			if (r.max_live <= ip) {
				mark_free(std::exchange(r.phys_reg, 0));
				return;
			}

			// Otherwise if register is still alive in the scope, allocate a register for the spiller,
			// and use it instead, effectively swapping use.
			//
			if (!noreg && ip < (1 + r.live.size()) && r.live[ip + 1]) {
				auto new_reg = get_anyreg(ip, owner, r.phys_reg > 0);
				move(new_reg, r.phys_reg);
				mark_free(std::exchange(r.phys_reg, 0));
				return;
			}

			// Spill to stack.
			//
			uint32_t spill_slot = 0;
			while (true) {
				if (spill_stack.size() == spill_slot) {
					spill_stack.emplace_back(owner);
					spill_slot = uint32_t(spill_stack.size() - 1);
					break;
				}
				if (spill_stack[spill_slot] < 0) {
					spill_stack[spill_slot] = owner;
					break;
				}
				++spill_slot;
			}
			r.spill_slot = spill_slot;
			store(r.phys_reg, spill_slot);
			mark_free(std::exchange(r.phys_reg, 0));
		}

		// Tries to allocate n'th scratch register for the caller.
		// Index is required as we do not save the attempts.
		//
		arch::reg alloc_next(uint32_t ip, bool gp, size_t index = 0, bool must_be_vol = false) {
			uint64_t  mask = gp ? allocated_gp_regs : allocated_fp_regs;
			size_t   limit;
			if (must_be_vol) {
				limit = gp ? std::size(arch::gp_volatile) : std::size(arch::fp_volatile);
			} else {
				limit = gp ? arch::num_gp_reg : arch::num_fp_reg;
			}

			arch::reg r    = 0;
			while (true) {
				r = std::countr_one(mask);
				if (r >= limit) {
					return 0;
				}
				if (!index--) {
					return gp ? r + 1 : -(r + 1);
				}
				mask &= ~(1ull << r);
			}
		}

		// Requests a specific register for the name.
		//
		bool get_reg(uint32_t ip, uint32_t name, arch::reg r) {
			// If the register is already allocated, we first have to spill the previous owner.
			//
			if (!is_free(r)) {
				for (uint32_t n = 0; n != vreg.size(); n++) {
					if (vreg[n].phys_reg == r) {
						spill(ip, n);
						break;
					}
				}
			}

			// Mark the register allocated.
			//
			mark_alloc(r);

			// If there was another register allocated:
			//
			auto& vr = vreg[name];
			if (vr.phys_reg) {
				// Move from it and then free it.
				//
				move(r, vr.phys_reg);
				mark_free(vr.phys_reg);
			}
			// Othewrise, if there is a spill slot:
			//
			else {
				// Load from the spill slot and free the slot.
				//
				if (vr.spill_slot >= 0) {
					load(r, vr.spill_slot);
					spill_stack[vr.spill_slot] = -1;
					vr.spill_slot              = -1;
				}
			}

			// Assign the final register and return.
			//
			vr.phys_reg = r;
			return r;
		}

		// Requests a register of given kind for the name.
		//
		arch::reg get_anyreg(uint32_t ip, uint32_t name, bool gp) {
			// Return if we already have a matching register for this.
			//
			auto& vr = vreg[name];
			if (vr.phys_reg != 0) {
				if ((vr.phys_reg > 0) == gp) {
					return vr.phys_reg;
				}
			}

			// Free expired registers.
			//
			for (auto& r : vreg) {
				if (r.max_live < ip) {
					mark_free(r.phys_reg);
					r.phys_reg = 0;
				}
			}

			// Try allocating the requested register kind.
			//
			auto r = alloc_next(ip, gp, 0, true);
			if (!r) {
				// We have to spill a register.
				//
				auto spill_cost = [&](register_state& reg) {
					return -(int32_t)reg.max_live;
				};

				uint32_t spilling        = UINT32_MAX;
				int32_t last_spill_cost = INT32_MAX;
				for (uint32_t n = 0; n != vreg.size(); n++) {
					// If same kind and dead:
					//
					if ((vreg[n].phys_reg > 0) == gp && !vreg[n].live[name]) {
						// Set to spill if cost is being minimized or its our only match so far.
						//
						int32_t cost = spill_cost(vreg[n]);
						if (spilling == UINT32_MAX || last_spill_cost > cost) {
							spilling = n;
							last_spill_cost = cost;
						}
					}
				}

				// Unless something horrible happens:!
				//
				if (spilling != UINT32_MAX) {
					// Spill to stack and try again.
					//
					spill(ip, spilling, true);
					r = alloc_next(ip, gp);
				}

				// How could this happen.
				//
				if (!r)
					return 0;
			}

			// Forward to exact allocator.
			//
			return get_reg(ip, name, r) ? r : 0;
		}
	};

	// Fills the interval information.
	//
	static void fill_intervals(reg_allocator& r) {
		for (auto& bb : r.proc->basic_blocks) {
			for (auto ins : *bb) {
				for (auto& op : ins->operands) {
					if (op->is<insn>()) {
						auto n                    = op->as<insn>()->name;
						r.vreg[n].live[ins->name] = true;
						r.vreg[n].max_live        = ins->name;
					}
				}
				if (ins->vt != type::none) {
					r.vreg[ins->name].live[ins->name] = true;
					r.vreg[ins->name].min_live        = ins->name;
				}
			}

			// Extend the intervals for block length.
			//
			for (uint32_t j = 0; j != r.vreg.size(); j++) {
				auto beg = bb->front()->name;
				auto end = bb->back()->name;
				while (beg != end && !r.vreg[j].live[beg])
					++beg;
				while (end != beg && !r.vreg[j].live[end])
					--end;
				for (size_t k = beg; k != end; k++) {
					r.vreg[j].live[k] = true;
				}
			}
		}
	}

	// Aliases casts and phi nodes.
	//
	static void alias_intervals(reg_allocator& r) {
		// Alias certain register names.
		//
		for (auto& bb : r.proc->basic_blocks) {
			for (auto ins : bb->insns()) {
				if (ins->alias) {
					auto alias_as = [&](insn* dst, insn* src) {
						r.merge(dst->name, src->name);
						dst->name = src->name;
					};

					// Use the name of the first insn.
					//
					alias_as(ins, ins->operands[0]->as<insn>());

					// For each other operand:
					//
					for (size_t i = 1; i != ins->operands.size(); i++) {
						if (ins->operands[i]->is<insn>()) {
							auto* s = ins->operands[i]->as<insn>()->parent;

							bool unique_src = true;
							for (size_t j = 0; j != ins->operands.size(); j++) {
								if (i != j && ins->operands[j]->is<insn>()) {
									auto* s2 = ins->operands[j]->as<insn>()->parent;
									if (s2 == s) {
										unique_src = false;
										break;
									}
								}
							}

							// If unique reference, rename them as well.
							//
							if (unique_src) {
								alias_as(ins->operands[i]->as<insn>(), ins);
							}
						}
					}
				}
			}
		}
	}

	static void alloc_regs(procedure* proc) {
		// Fixup PHIs.
		//
		for (auto& bb : proc->basic_blocks) {
			for (auto phi : bb->phis()) {
				for (size_t i = 0; i != phi->operands.size(); i++) {
					if (phi->operands[i]->is<constant>()) {
						phi->operands[i] = builder{}.emit_before<move>(bb->predecesors[i]->back(), phi->operands[i]);
					}
				}
			}
		}

		// Topologically sort the procedure and rename every register.
		//
		proc->reset_names();

		// Create and fill interval information.
		//
		reg_allocator r{proc};
		fill_intervals(r);
		alias_intervals(r);

		// Print the intervals.
		//
		r.print_intervals();

		// Demo.
		//
		printf("\n");
		uint32_t index = 0;
		for (auto& bb : proc->basic_blocks) {
			printf("-- Block $%u", bb->uid);
			if (bb->cold_hint)
				printf(LI_CYN " [COLD %u]" LI_DEF, (uint32_t) bb->cold_hint);
			putchar('\n');

			for (auto i : *bb) {
				arch::reg result_reg = -999;
				if (i->vt != type::none) {
					result_reg = r.get_anyreg(index, i->name, true);
				}

				std::vector<arch::reg> arg_regs = {};
				if (!i->is<compare>()) {
					for (auto& op : i->operands) {
						if (!op->is<constant>()) {
							arg_regs.emplace_back(r.get_anyreg(index, op->as<insn>()->name, true));
						} else {
							arg_regs.emplace_back(-999);
						}
					}
				}
				printf(LI_GRN "#%-5x" LI_DEF " %s {%02x}", i->source_bc, i->to_string(true).c_str(), index);
				if (result_reg != -999)
					printf(" # => R%s", arch::name_native(arch::to_native(result_reg)));
				else
					printf(" # => none");
				for (size_t k = 0; k != arg_regs.size(); k++) {
					if (arg_regs[k] != -999) {
						printf(" - R%s", arch::name_native(arch::to_native(arg_regs[k])));
					}
				}
				putchar('\n');
				++index;
			}
		}
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