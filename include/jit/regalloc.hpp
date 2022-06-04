#pragma once
#include <vector>
#include <util/common.hpp>
#include <util/format.hpp>
#include <ir/proc.hpp>
#include <ir/insn.hpp>
#include <ir/arch.hpp>
#include <util/func.hpp>

// Implements the register allocator to be used by the JIT backends.
// - This file is meant to be included by a single compilation unit so everything is inline.
//
namespace li::ir::jit {
	// State of a named register.
	//
	struct register_state {
		// Cost of bad allocation.
		//
		uint32_t cost = 1;

		// Live interval and cached min-max.
		//
		std::vector<bool> live;
		uint32_t          max_live = 0;
		uint32_t          min_live = 0;

		// Current register and spill slot.
		//
		arch::reg phys_reg   = arch::reg_none;
		int32_t   spill_slot = -1;
	};

	// State of register allocator.
	//
	struct reg_allocator {
		// Relevant procedure.
		//
		procedure*                  proc;

		// State of virtual registers.
		//
		std::vector<register_state> vreg;

		// Mask of register allocations.
		//
		uint64_t active_gp_regs            = 0;
		uint64_t active_fp_regs            = 0;
		uint64_t cumilative_gp_reg_history = 0;
		uint64_t cumilative_fp_reg_history = 0;

		// Stack for spilled values.
		//
		std::vector<int32_t>        spill_stack       = {};

		// Callbacks to code-generator for issuing arch-indep instructions.
		//
		// - Stack[N*sizeof void*] = Reg
		util::function_view<void(arch::reg, uint32_t)> store;
		// - Reg = Stack[N*sizeof void*]
		util::function_view<void(arch::reg, uint32_t)> load;
		// - Reg1 = Reg2
		util::function_view<void(arch::reg, arch::reg)> move;

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
			vreg[src].cost     = vreg[src].cost + vreg[dst].cost;
			vreg[dst].min_live = 0;
			vreg[dst].max_live = 0;
			vreg[dst].live.clear();
			vreg[dst].live.resize(vreg.size(), false);
		}

		// Gets the currently associated reg with the name if there is one.
		//
		arch::reg check_reg(uint32_t name) const {
			return vreg[name].phys_reg;
		}

		// Marks a register allocated/freed.
		//
		void mark_alloc(arch::reg r) {
			if (r > 0)
				active_gp_regs |= 1ull << (r - 1);
			else if (r < 0)
				active_fp_regs |= 1ull << (-(r + 1));
			else
				util::abort("allocating null register.");

			cumilative_gp_reg_history |= active_gp_regs;
			cumilative_fp_reg_history |= active_fp_regs;
		}
		void mark_free(arch::reg r) {
			if (r > 0)
				active_gp_regs &= ~(1ull << (r - 1));
			else if (r < 0)
				active_fp_regs &= ~(1ull << (-(r + 1)));
		}
		bool is_free(arch::reg r) const {
			if (r > 0)
				return active_gp_regs & (1ull << (r - 1));
			else if (r < 0)
				return active_fp_regs & (1ull << (-(r + 1)));
			else
				util::abort("testing null register.");
		}

		// Spills a virtual register.
		//
		void spill(uint32_t ip, uint32_t owner, bool noreg = false) {
			// If register is dead after this, ignore.
			//
			auto& r = vreg[owner];
			if (r.max_live <= ip) {
				mark_free(std::exchange(r.phys_reg, arch::reg_none));
				return;
			}

			// Otherwise if register is still alive in the scope, allocate a register for the spiller,
			// and use it instead, effectively swapping use.
			//
			if (!noreg && ip < (1 + r.live.size()) && r.live[ip + 1]) {
				auto new_reg = get_anyreg(ip, owner, r.phys_reg > 0);
				move(new_reg, r.phys_reg);
				mark_free(std::exchange(r.phys_reg, arch::reg_none));
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
			mark_free(std::exchange(r.phys_reg, arch::reg_none));
		}

		// Spills a physical register on demand.
		//
		void spill_arch(uint32_t ip, arch::reg r) {
			if (!is_free(r)) {
				for (uint32_t n = 0; n != vreg.size(); n++) {
					if (vreg[n].phys_reg == r) {
						spill(ip, n);
						break;
					}
				}
			}
		}

		// Tries to allocate n'th scratch register for the caller, index is required as we do not
		// save information about each attempt.
		//
		arch::reg alloc_next(uint32_t ip, bool gp, size_t index = 0, bool must_be_vol = false) {
			uint64_t mask = gp ? active_gp_regs : active_fp_regs;
			size_t   limit;
			if (must_be_vol) {
				limit = gp ? std::size(arch::gp_volatile) : std::size(arch::fp_volatile);
			} else {
				limit = gp ? arch::num_gp_reg : arch::num_fp_reg;
			}

			arch::reg r = arch::reg_none;
			while (true) {
				r = (arch::reg)std::countr_one(mask);
				if (r >= limit) {
					return arch::reg_none;
				}
				if (!index--) {
					if (gp) cumilative_gp_reg_history |= 1ull << r;
					else    cumilative_fp_reg_history |= 1ull << r;

					return gp ? arch::reg(r + 1) : arch::reg(-(r + 1));
				}
				mask &= ~(1ull << r);
			}
		}

		// Requests a specific register for the name.
		//
		bool get_reg(uint32_t ip, uint32_t name, arch::reg r, bool discard_value = false) {
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
				if (!discard_value)
					move(r, vr.phys_reg);
				mark_free(vr.phys_reg);
			}
			// Othewrise, if there is a spill slot:
			//
			else {
				// Load from the spill slot and free the slot.
				//
				if (vr.spill_slot >= 0) {
					if (!discard_value)
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
		arch::reg get_anyreg(uint32_t ip, uint32_t name, bool gp, bool discard_value = false) {
			discard_value = false;

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
					r.phys_reg = arch::reg_none;
				}
			}

			// Try allocating the requested register kind.
			//
			auto r = alloc_next(ip, gp, 0, true);
			if (!r) {
				// We have to spill a register.
				//
				auto spill_cost = [&](register_state& reg) { return reg.cost + int32_t((ip - reg.max_live) << 4); };

				uint32_t spilling        = UINT32_MAX;
				int32_t  last_spill_cost = INT32_MAX;
				for (uint32_t n = 0; n != vreg.size(); n++) {
					// If same kind and dead:
					//
					if ((vreg[n].phys_reg > 0) == gp && !vreg[n].live[name]) {
						// Set to spill if cost is being minimized or its our only match so far.
						//
						int32_t cost = spill_cost(vreg[n]);
						if (spilling == UINT32_MAX || last_spill_cost > cost) {
							spilling        = n;
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
					return arch::reg_none;
			}

			// Forward to exact allocator.
			//
			return get_reg(ip, name, r, discard_value) ? r : arch::reg_none;
		}

		// If caller does not care about the register type, it should call this function to
		// determine which type is more preferable to allocate here.
		//
		bool ideal_reg_type(uint32_t name = UINT32_MAX) const {
			// If register is named and we have a physical register associated with it, preferably request the same type.
			//
			if (name != UINT32_MAX && vreg[name].phys_reg) {
				return vreg[name].phys_reg >= 0;
			}

			// Otherwise pick based on most volatile regs available.
			//
			auto num_gp_available = std::popcount(~active_gp_regs & util::fill_bits((int) std::size(arch::gp_volatile)));
			auto num_fp_available = std::popcount(~active_fp_regs & util::fill_bits((int) std::size(arch::fp_volatile)));
			if (num_gp_available != num_fp_available) {
				return num_gp_available > num_fp_available;
			}
			num_gp_available = std::popcount(~active_gp_regs & util::fill_bits(arch::num_gp_reg));
			num_fp_available = std::popcount(~active_fp_regs & util::fill_bits(arch::num_fp_reg));
			if (num_gp_available != num_fp_available) {
				return num_gp_available > num_fp_available;
			}
			return true;
		}

		// Prints interval graph.
		//
		void print() const {
			printf("   ");
			for (uint32_t j = 0; j != vreg.size(); j++) {
				printf("|%02x", j);
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
	};

	// Pre-register allocation cleanup pass.
	//
	static void pre_alloc_cleanup(procedure* proc) {
		// Fixup PHIs to not have any constants.
		//
		for (auto& bb : proc->basic_blocks) {
			for (auto phi : bb->phis()) {
				for (size_t i = 0; i != phi->operands.size(); i++) {
					if (phi->operands[i]->is<constant>()) {
						phi->operands[i] = builder{}.emit_before<move>(bb->predecessors[i]->back(), phi->operands[i]);
					}
				}
			}
		}

		// Topologically sort the procedure and rename every register.
		//
		proc->topological_sort();
		proc->reset_names();
	}

	// Fills the interval information.
	//
	static void fill_intervals(reg_allocator& r) {
		// Get basic loop info.
		//
		for (auto& bb : r.proc->basic_blocks) {
			bb->loop_depth = bb->check_path(bb.get()) ? 1 : 0;
		}

		// For each block.
		//
		for (auto& bb : r.proc->basic_blocks) {
			// Update live mask for each use.
			//
			for (auto ins : *bb) {
				for (auto& op : ins->operands) {
					if (op->is<insn>()) {
						auto n                    = op->as<insn>()->name;
						r.vreg[n].live[ins->name] = true;
						r.vreg[n].max_live        = ins->name;
						r.vreg[n].cost += bb->loop_depth + 1;
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

	// Coalesces casts and phi nodes.
	//
	static void coalesce_intervals(reg_allocator& r) {
		// Coalesce certain register names.
		//
		for (auto& bb : r.proc->basic_blocks) {
			for (auto ins : bb->insns()) {
				if (ins->alias) {
					auto coalesce_as = [&](insn* dst, insn* src) {
						printf("merge %u <= %u [%s]\n", dst->name, src->name, dst->name <= src->name ? "BAD" : "");
						//if (dst->name <= src->name)
						//	return;
						r.merge(dst->name, src->name);
						dst->name = src->name;
					};
		
					if (ins->is<phi>()) {
						coalesce_as(ins, ins->operands[0]->as<insn>());
						for (size_t i = 1; i != ins->operands.size(); i++) {
							coalesce_as(ins->operands[i]->as<insn>(), ins);
						}
					} else {
						for (size_t i = 0; i != ins->operands.size(); i++) {
							if (ins->operands[i]->is<insn>()) {
								coalesce_as(ins, ins->operands[i]->as<insn>());
								break;
							}
						}
					}
				}
			}
		}
	}

	// Creates an allocator after completing all required passes.
	//
	static reg_allocator init_regalloc(procedure* proc) {
		pre_alloc_cleanup(proc);
		reg_allocator r{proc};
		fill_intervals(r);
		coalesce_intervals(r);
		return r;
	}
};