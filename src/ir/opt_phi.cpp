#include <algorithm>
#include <functional>
#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace li::ir::opt {
	/*
	* The following algorithm is a modified version adapted from the paper:
		Braun, M., Buchwald, S., Hack, S., Leißa, R., Mallon, C., Zwinkau, A. (2013). Simple and Efficient Construction of Static Single Assignment Form. In:
	Jhala, R., De Bosschere, K. (eds) Compiler Construction. CC 2013. Lecture Notes in Computer Science, vol 7791. Springer, Berlin, Heidelberg.
	*/
	// Resolves trivial PHIs to a fixed point over the PHI graph alone, then rewrites
	// every operand in one procedure scan. Per-PHI user scans are quadratic in the
	// procedure and made lazily lowered control flow take tens of seconds to compile.
	static void remove_trivial_phis(procedure* proc) {
		std::unordered_map<value*, value*> replacement;
		auto                               resolve = [&](value* candidate) {
			while (true) {
				auto it = replacement.find(candidate);
				if (it == replacement.end())
					return candidate;
				candidate = it->second;
			}
		};

		std::vector<insn*> phis;
		for (auto& block : proc->basic_blocks)
			for (auto* value : block->phis())
				phis.emplace_back(value);

		bool changed;
		do {
			changed = false;
			for (auto* candidate : phis) {
				if (replacement.contains(candidate))
					continue;
				value* same    = nullptr;
				bool   trivial = true;
				for (auto& op : candidate->operands) {
					value* operand = resolve(op.get());
					if (operand == same || operand == candidate)
						continue;
					if (same) {
						trivial = false;
						break;
					}
					same = operand;
				}
				if (trivial && same) {
					replacement.emplace(candidate, same);
					changed = true;
				}
			}
		} while (changed);
		if (replacement.empty())
			return;

		for (auto& block : proc->basic_blocks) {
			for (auto* instruction : block->insns()) {
				if (replacement.contains(instruction))
					continue;
				bool rewritten = false;
				for (auto& op : instruction->operands) {
					if (!replacement.contains(op.get()))
						continue;
					op        = make_use(resolve(op.get()));
					rewritten = true;
				}
				if (rewritten)
					instruction->update();
			}
		}
		for (auto* candidate : phis) {
			if (replacement.contains(candidate)) {
				// A trivial PHI may reference itself; drop its uses so it is freed.
				ref<insn> owner = make_ref(candidate);
				owner->operands.clear();
				owner->erase();
			}
		}
	}

	struct phi_lifter {
		procedure* proc;

		// The cached value is the register's value on entry to the block. Install
		// provisional PHIs in this cache before following predecessor edges:
		// this both breaks loops and prevents repeated reads through a diamond
		// from rebuilding the same SSA subgraph exponentially.
		//
		std::unordered_map<basic_block*, std::unordered_map<bc::reg, ref<>>> entry_values;

		ref<> read_variable_local(bc::reg r, basic_block* b, insn* until = nullptr) {
			for (insn* ins : view::reverse(b->before(until))) {
				if (ins->is<store_local>() && ins->operands[0]->as<constant>()->i32 == r)
					return ins->operands[1];
			}
			return nullptr;
		}

		ref<> reread_entry_variable(bc::reg r, basic_block* b) {
			for (insn* ins : view::reverse(b->insns())) {
				if (ins->is<load_local>() && ins->operands[0]->as<constant>()->i32 == r)
					return make_ref(ins);
			}
			return nullptr;
		}

		ref<> read_variable(bc::reg r, basic_block* b, insn* until = nullptr) {
			auto v = read_variable_local(r, b, until);
			if (!v)
				v = read_variable_recursive(r, b);
			return v;
		}

		ref<> read_variable_recursive(bc::reg r, basic_block* b) {
			auto& cached = entry_values[b];
			if (auto existing = cached.find(r); existing != cached.end())
				return existing->second;

			// Arguments come from the caller; fresh locals and pending-call
			// slots have no owned value before their first definition.
			if (b == proc->get_entry()) {
				ref<> value;
				if (r >= 0) {
					value = launder_value(proc, nil);
				} else {
					value = reread_entry_variable(r, b);
					if (!value)
						value = builder{b}.emit_front<load_local>(r);
				}
				cached.emplace(r, value);
				return value;
			}

			// Unreachable blocks are removed before lifting. Keep construction
			// finite if a malformed caller nevertheless supplies a detached block;
			// the full verifier still reports the invalid CFG.
			//
			if (b->predecessors.empty()) {
				auto value = builder{b}.emit_front<load_local>(r);
				cached.emplace(r, value);
				return value;
			}

			ref<insn> value = builder{b}.emit_front<phi>();
			cached.emplace(r, value);
			for (auto* predecessor : b->predecessors)
				value->operands.emplace_back(read_variable(r, predecessor));
			value->update();
			return value;
		}
	};

	// Lowers load/store of locals to PHI nodes and named registers.
	//
	void lift_phi(procedure* proc) {
		proc->remove_unreachable_blocks();
		phi_lifter lifter{proc};

		// A negative register is caller-owned frame storage. If bytecode writes
		// one, every access to that register must remain physical: erasing the
		// store would keep the old argument alive until frame exit and omit the
		// new slot owner entirely.
		std::unordered_set<bc::reg> materialized_frame_slots;
		for (auto& bb : proc->basic_blocks) {
			for (auto* ins : bb->insns()) {
				if (ins->is<store_local>()) {
					auto r = ins->operands[0]->as<constant>()->i32;
					if (r < 0)
						materialized_frame_slots.emplace(r);
				}
			}
		}
		if (!materialized_frame_slots.empty()) {
			for (auto& bb : proc->basic_blocks) {
				for (auto* ins : bb->insns()) {
					if (ins->is<load_local>() || ins->is<store_local>()) {
						auto r = ins->operands[0]->as<constant>()->i32;
						if (materialized_frame_slots.contains(r))
							ins->is_volatile = true;
					}
				}
			}
		}

		// Generate PHIs to replace load_local in every block except the entry point.
		//
		for (auto& bb : view::reverse(proc->basic_blocks)) {
			if (bb.get() != proc->get_entry()) {
				bb->erase_if([&](insn* ins) {
					if (ins->is<load_local>() && !ins->is_volatile) {
						bc::reg r = ins->operands[0]->as<constant>()->i32;
						ins->replace_all_uses(lifter.read_variable(r, bb.get(), ins));
						return true;
					}
					return false;
				});
			}
		}

		// A lifted slot owns its current SSA value for exactly as long as the
		// corresponding frame slot did. Preserve the value being replaced at
		// each erased store so ownership cannot shorten its lifetime to an
		// arbitrary last SSA use.
		//
		std::unordered_set<bc::reg> reference_slots;
		for (auto& bb : proc->basic_blocks) {
			for (auto* ins : bb->insns()) {
				if (!ins->is<store_local>() || ins->is_volatile)
					continue;
				auto r = ins->operands[0]->as<constant>()->i32;
				auto v = ins->operands[1].get();

				// Pending-call slots are consumed by CALL and reused by later
				// PUSH instructions. Their SSA operands carry ownership through
				// the call; treating a later reuse as a local-slot overwrite
				// would keep the consumed argument alive past the call.
				if (r >= bc::reg(proc->f->num_locals))
					continue;

				if (v->vt == type::any || is_gc_data(v->vt))
					reference_slots.emplace(r);
				auto previous = lifter.read_variable(r, bb.get(), ins);
				if (previous->vt == type::any || is_gc_data(previous->vt))
					builder{ins}.emit_before<keep_alive>(ins, previous);
			}
		}

		// Returning or propagating an uncaught exception tears down the remaining
		// frame slots. Emit the markers in descending slot order to match the
		// interpreter's stack truncation order. Overwritten values are already
		// bounded by the store markers above, so this does not extend every
		// allocation to function exit.
		//
		std::vector<bc::reg> cleanup_slots(reference_slots.begin(), reference_slots.end());
		std::sort(cleanup_slots.begin(), cleanup_slots.end(), std::greater<>{});
		for (auto& bb : proc->basic_blocks) {
			auto* terminator = bb->back();
			if (!terminator->is<ret>())
				continue;
			for (auto r : cleanup_slots) {
				auto current = lifter.read_variable(r, bb.get(), terminator);
				if (current->vt == type::any || is_gc_data(current->vt))
					builder{terminator}.emit_before<keep_alive>(terminator, current);
			}
		}

		// Cached entry values are no longer consulted after lifetime markers have
		// been placed. Drop the cache before erasing trivial PHIs so no detached
		// instruction can remain reachable through a stale entry.
		lifter.entry_values.clear();
		remove_trivial_phis(proc);

		// Remove all non-materialized stores.
		//
		for (auto& bb : proc->basic_blocks) {
			bb->erase_if([&](insn* ins) { return ins->is<store_local>() && !ins->is_volatile; });
		}
		proc->validate();
	}
};