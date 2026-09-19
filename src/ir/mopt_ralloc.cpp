#include <ir/opt.hpp>
#include <unordered_map>
#include <unordered_set>
#include <util/common.hpp>
#if LI_JIT

	#ifndef LI_RA_TEST_PRESSURE
		#define LI_RA_TEST_PRESSURE 0
	#endif

namespace li::ir::opt {
	static constexpr float RA_PRIO_HOT_BIAS = 12.0f;

	struct graph_node {
		std::vector<msize_t> neighbors;
		float                priority            = 0;
		float                spill_priority      = 0;
		intptr_t             coalescing_hints[4] = {0};  // offset
		uint8_t              hint_id             = 0;
		uint8_t              color               = {};
		bool                 is_fp               = false;
		bool                 present             = false;
		int32_t              spill_slot          = 0;

		void add_hint(graph_node* g) { coalescing_hints[hint_id++ % std::size(coalescing_hints)] = g - this; }
	};

	template<typename F>
	static void for_each_reg_in(const util::bitset& bs, F&& fn) {
		for (size_t block = 0; block != bs.data.size(); block++) {
			size_t bits = bs.data[block];
			while (bits) {
				size_t bit = std::countr_zero(bits);
				fn(mreg::from_uid(msize_t(block * util::bitset::width + bit)));
				bits &= bits - 1;
			}
		}
	}

	// Returns true if the register does not require allocation.
	//
	static bool is_pseudo(mreg r) { return r.is_virt() && r.virt() > 0 && r.virt() < vreg_first; }

	// Returns true if register should be included in the interference graph.
	//
	static bool interferes_with(mreg a, mreg b) {
		// Ignore pseudo registers.
		//
		if (is_pseudo(a) || is_pseudo(b))
			return false;

		// Match target class.
		//
		if (a.is_fp() != b.is_fp())
			return false;
		return true;
	}

	// Tries coloring the graph with the supplied (possibly holey) physical-ID
	// masks. Reserved registers are never candidates and do not reduce the
	// degree threshold merely because their architectural IDs lie below it.
	//
	static std::pair<size_t, size_t> try_color(std::span<graph_node> gr, uint64_t gp_colors, uint64_t fp_colors) {
		std::vector<size_t> degrees(gr.size());
		std::vector<bool>   active(gr.size(), true);
		std::vector<size_t> simplify_stack;
		std::vector<size_t> low_degree;
		std::vector<size_t> spill_heap;
		simplify_stack.reserve(gr.size());
		low_degree.reserve(gr.size());
		spill_heap.reserve(gr.size());

		auto colors_for    = [&](size_t i) { return gr[i].is_fp ? fp_colors : gp_colors; };
		auto limit_for     = [&](size_t i) { return size_t(std::popcount(colors_for(i))); };
		auto is_precolored = [&](size_t i) { return mreg::from_uid(msize_t(i)).is_phys(); };
		auto spill_cmp     = [&](size_t a, size_t b) {
			if (gr[a].spill_priority != gr[b].spill_priority)
				return gr[a].spill_priority > gr[b].spill_priority;
			return a > b;
		};
		auto counts_toward_degree = [&](size_t owner, size_t neighbor) {
			if (owner == neighbor || !gr[neighbor].present)
				return false;
			if (!is_precolored(neighbor))
				return true;
			uint8_t color = gr[neighbor].color;
			return color && (colors_for(owner) & (uint64_t{1} << (color - 1)));
		};

		for (size_t i = 0; i != gr.size(); i++) {
			if (!gr[i].present) {
				active[i] = false;
				continue;
			}
			if (!is_precolored(i)) {
				gr[i].color      = 0;
				gr[i].spill_slot = 0;
			}

			size_t degree = 0;
			for (msize_t neighbor : gr[i].neighbors)
				degree += counts_toward_degree(i, neighbor);
			degrees[i] = degree;
			if (is_precolored(i))
				continue;
			if (degrees[i] < limit_for(i))
				low_degree.push_back(i);
			if (gr[i].priority != std::numeric_limits<float>::infinity())
				spill_heap.push_back(i);
		}
		std::reverse(low_degree.begin(), low_degree.end());
		std::make_heap(spill_heap.begin(), spill_heap.end(), spill_cmp);

		size_t remaining = 0;
		for (size_t i = 0; i != gr.size(); i++)
			remaining += gr[i].present && !is_precolored(i);

		while (remaining) {
			size_t chosen = gr.size();
			while (!low_degree.empty()) {
				size_t candidate = low_degree.back();
				low_degree.pop_back();
				if (active[candidate] && !is_precolored(candidate) && degrees[candidate] < limit_for(candidate)) {
					chosen = candidate;
					break;
				}
			}

			while (chosen == gr.size() && !spill_heap.empty()) {
				std::pop_heap(spill_heap.begin(), spill_heap.end(), spill_cmp);
				size_t candidate = spill_heap.back();
				spill_heap.pop_back();
				if (active[candidate] && !is_precolored(candidate) && degrees[candidate] >= limit_for(candidate))
					chosen = candidate;
			}
			if (chosen == gr.size())
				util::abort("register allocation cannot spill a constrained instruction");

			active[chosen] = false;
			remaining--;
			simplify_stack.push_back(chosen);

			for (msize_t i : gr[chosen].neighbors) {
				if (i == chosen || !active[i] || !gr[i].present || is_precolored(i))
					continue;
				LI_ASSERT(degrees[i] != 0);
				degrees[i]--;
				if (degrees[i] < limit_for(i))
					low_degree.push_back(i);
			}
		}

		size_t spill_gp = 0;
		size_t spill_fp = 0;
		for (auto stack_it = simplify_stack.rbegin(); stack_it != simplify_stack.rend(); ++stack_it) {
			size_t      idx        = *stack_it;
			graph_node& node       = gr[idx];
			uint64_t    color_mask = colors_for(idx);

			for (msize_t neighbor : node.neighbors) {
				uint8_t color = gr[neighbor].color;
				if (color)
					color_mask &= ~(uint64_t{1} << (color - 1));
			}

			for (intptr_t hint_off : node.coalescing_hints) {
				if (!hint_off)
					continue;
				auto*   hint  = &node + hint_off;
				uint8_t color = hint->color;
				if (color && (color_mask & (uint64_t{1} << (color - 1)))) {
					node.color = color;
					break;
				}
			}
			if (node.color)
				continue;

			if (color_mask) {
				node.color = uint8_t(std::countr_zero(color_mask) + 1);
				continue;
			}
			if (node.priority == std::numeric_limits<float>::infinity())
				util::abort("register allocation cannot spill a constrained instruction");

			if (node.is_fp)
				spill_fp++;
			else
				spill_gp++;

			node.spill_slot = -1;
		}

		return {spill_gp, spill_fp};
	}

	// A hard tied operand is represented as one read/write virtual register. The
	// surrounding moves remain coalescing candidates, but correctness no longer
	// depends on the colorer's preference being satisfiable.
	//
	static void normalize_tied_constraints(mprocedure* proc) {
		for (auto& bb : proc->basic_blocks) {
			for (size_t index = 0; index != bb.instructions.size(); index++) {
				minsn ins = bb.instructions[index];
				if (ins.tied_arg == minsn::no_arg)
					continue;
				if (!ins.out || ins.tied_arg >= ins.num_args() || !ins.arg[ins.tied_arg].is_reg() || ins.out.is_fp() != ins.arg[ins.tied_arg].reg.is_fp()) {
					util::abort("invalid MIR tied-operand constraint");
				}

				mreg source = ins.arg[ins.tied_arg].reg;
				if (source == ins.out) {
					bb.instructions[index].tied_arg = minsn::no_arg;
					continue;
				}

				mreg tied              = source.is_fp() ? proc->next_fp() : proc->next_gp();
				ins.arg[ins.tied_arg]  = tied;
				mreg original_out      = ins.out;
				ins.out                = tied;
				ins.tied_arg           = minsn::no_arg;
				bb.instructions[index] = minsn{source.is_fp() ? vop::movf : vop::movi, tied, source};
				bb.instructions.insert(bb.instructions.begin() + index + 1, ins);
				bb.instructions.insert(bb.instructions.begin() + index + 2, minsn{original_out.is_fp() ? vop::movf : vop::movi, original_out, tied});
				index += 2;
			}
		}
	}

	// Spills all arguments into virtual registers.
	//
	static void spill_args(mprocedure* proc) {
		// Before anything else, spill all arguments into virtual registers.
		//
		mreg    regs[3]    = {};
		int32_t tos_offset = 8 + proc->max_stack_slot * 8;
		for (auto& bb : proc->basic_blocks) {
			for (auto& i : bb.instructions) {
				// Alias ToS into args with offset.
				//
				for (auto& arg : i.arg) {
					if (!arg.is_mem())
						continue;
					if (arg.mem.base == vreg_tos) {
						arg.mem.base = vreg_args;
						arg.mem.disp += tos_offset;
					}
					if (arg.mem.index == vreg_tos) {
						arg.mem.index = vreg_args;
						arg.mem.disp += tos_offset * (int32_t{1} << arg.mem.shift);
					}
				}

				i.for_each_reg([&](const mreg& r, bool is_read) {
					mreg* replace_with = nullptr;
					if (r == vreg_vm)
						replace_with = &regs[0];
					else if (r == vreg_args)
						replace_with = &regs[1];
					else if (r == vreg_nargs)
						replace_with = &regs[2];
					if (replace_with) {
						if (replace_with->is_null()) {
							*replace_with = proc->next_gp();
						}
						const_cast<mreg&>(r) = *replace_with;
					}
				});
			}
		}
		for (size_t i = 0; i != std::size(regs); i++) {
			if (regs[i]) {
				proc->basic_blocks.front().instructions.insert(
					 proc->basic_blocks.front().instructions.begin(), minsn{vop::movi, regs[i], mreg(arch::map_gp_arg(i, 0))});
			}
		}
	}

	// Does lifetime analysis and builds the interference graph.
	//
	static std::vector<graph_node> build_graph(mprocedure* proc, std::vector<graph_node>* tmp = nullptr) {
		// Get maximum register id and calculate use counts.
		//
		std::vector<float> reg_prios   = {};
		std::vector<bool>  reg_present = {};
		msize_t            max_reg_id  = 0;
		for (auto& bb : proc->basic_blocks) {
			for (auto& i : bb.instructions) {
				i.for_each_reg_w_implicit([&](mreg r, bool is_read) {
					msize_t uid = r.uid();
					if (max_reg_id < uid)
						max_reg_id = uid;
					if (reg_prios.size() <= uid) {
						reg_prios.resize(uid + 1);
						reg_present.resize(uid + 1);
					}
					reg_present[uid] = true;
					if (i.no_spill)
						reg_prios[uid] = std::numeric_limits<float>::infinity();
					else if (is_read)
						reg_prios[uid] += (bb.hot * RA_PRIO_HOT_BIAS) + 1;
				});
			}
		}
		++max_reg_id;
		reg_prios.resize(max_reg_id);
		reg_present.resize(max_reg_id);

		// First calculate ref(n) and def(n) for each basic block.
		//
		for (auto& bb : proc->basic_blocks) {
			bb.df_def.clear();
			bb.df_ref.clear();
			bb.df_in_live.clear();
			bb.df_out_live.clear();
			bb.df_def.resize(max_reg_id);
			bb.df_ref.resize(max_reg_id);
			bb.df_in_live.resize(max_reg_id);
			bb.df_out_live.resize(max_reg_id);

			for (auto& i : bb.instructions) {
				i.for_each_reg_w_implicit([&](mreg r, bool is_read) {
					if (is_pseudo(r))
						return;

					if (is_read) {
						if (!bb.df_def[r.uid()]) {
							// Ignore call arguments that are not defined in this block.
							if (r.is_phys() && i.is(vop::call))
								return;
							bb.df_ref.set(r.uid());
						}
					} else {
						bb.df_def.set(r.uid());
					}
				});
			}
		}

		// Calculate in-live ranges:
		// - in-live(n) = (out-live(n)\def(n)) U ref(n)
		// - out-live(n) = for each succ, (... U in-live(s))
		//
		// Liveness flows backwards, so visiting blocks in reverse layout order
		// converges in a few sweeps instead of one sweep per CFG depth. The
		// scratch set is reused: allocating one per block per sweep dominated
		// compile time on large scripts.
		util::bitset new_live{max_reg_id};
		bool         changed;
		do {
			changed = false;
			for (auto& bb : proc->basic_blocks | std::views::reverse) {
				new_live.fill(false);
				for (auto& s : bb.successors) {
					new_live.set_union(s->df_in_live);
				}
				new_live.set_difference(bb.df_def);
				new_live.set_union(bb.df_ref);
				if (new_live != bb.df_in_live) {
					changed = true;
					new_live.swap(bb.df_in_live);
				}
			}
		} while (changed);

		// Convert to out-live.
		//
		for (auto& b : proc->basic_blocks) {
			for (auto& suc : b.successors)
				b.df_out_live.set_union(suc->df_in_live);
		}

		// Allocate the interference graph and set the initial state.
		//
		std::vector<graph_node> interference_graph;
		if (tmp) {
			interference_graph = std::move(*tmp);
			interference_graph.clear();
		}
		// Only actual edges occupy storage. Dense adjacency rows cost quadratic
		// memory and scans even when each register has only a few neighbors.
		interference_graph.resize(max_reg_id);
		for (msize_t i = 0; i != max_reg_id; i++) {
			auto& node    = interference_graph[i];
			auto  mr      = mreg::from_uid(i);
			node.priority = reg_prios[i];
			node.is_fp    = mr.is_fp();
			node.present  = reg_present[i] && !is_pseudo(mr);
			if (!node.present)
				continue;
			node.neighbors.push_back(i);
			if (mr.is_phys())
				node.color = uint8_t(std::abs(int32_t(mr.phys())));
		}

		// Build the interference graph.
		//
		std::unordered_set<uint64_t> edges;
		auto                         add_vertex = [&](mreg a, mreg b) {
			if (a == b || !interferes_with(a, b))
				return;
			const auto     au   = a.uid();
			const auto     bu   = b.uid();
			const uint64_t edge = (uint64_t(std::min(au, bu)) << 32) | std::max(au, bu);
			if (edges.insert(edge).second) {
				interference_graph[au].neighbors.push_back(bu);
				interference_graph[bu].neighbors.push_back(au);
			}
		};
		auto add_set = [&](const std::unordered_set<msize_t>& live, mreg def) {
			for (msize_t uid : live)
				add_vertex(def, mreg::from_uid(uid));
		};
		auto add_hint = [&](mreg a, mreg b) {
			if (a == b || a.is_fp() != b.is_fp())
				return;
			auto& an = interference_graph[a.uid()];
			auto& bn = interference_graph[b.uid()];
			an.add_hint(&bn);
			bn.add_hint(&an);
		};
		for (auto& b : proc->basic_blocks) {
			std::unordered_set<msize_t> live;
			for_each_reg_in(b.df_out_live, [&](mreg r) { live.insert(r.uid()); });
			for (auto& i : view::reverse(b.instructions)) {
				if (i.is_move_between_same_class())
					add_hint(i.arg[0].reg, i.out);
				if (i.out && i.hint_arg < i.num_args() && i.arg[i.hint_arg].is_reg())
					add_hint(i.arg[i.hint_arg].reg, i.out);

				i.for_each_reg_w_implicit([&](mreg r, bool is_read) {
					if (!is_read) {
						live.erase(r.uid());
						add_set(live, r);
					}
				});
				i.for_each_reg_w_implicit([&](mreg r, bool is_read) {
					if (is_read) {
						// Ignore call arguments that are not defined in this block.
						if (r.is_phys() && i.is(vop::call)) {
							auto written = &i != std::find_if(b.instructions.data(), &i, [&](minsn& i) { return i.writes_to_register(r); });
							if (!written)
								return;
						}
						live.insert(r.uid());
					}
				});
				i.for_each_reg_w_implicit([&](mreg r, bool is_read) {
					if (is_read) {
						// Ignore call arguments that are not defined in this block.
						if (r.is_phys() && i.is(vop::call)) {
							auto written = &i != std::find_if(b.instructions.data(), &i, [&](minsn& i) { return i.writes_to_register(r); });
							if (!written)
								return;
						}
						add_set(live, r);
					}
				});
			}
		}
		for (auto& node : interference_graph) {
			// Stable neighbor order preserves coloring decisions across hash-table
			// implementations, platforms, and allocation addresses.
			std::sort(node.neighbors.begin(), node.neighbors.end());
			// Interference pressure is a stable proxy for live-range length. Dividing
			// use benefit by it makes cheap, long-lived values the preferred spills.
			auto pressure       = node.neighbors.size() > 1 ? node.neighbors.size() - 1 : size_t(1);
			node.spill_priority = node.priority / float(pressure);
		}
		return interference_graph;
	}

	// Coalescing can map many independent SSA cleanup values to one physical
	// register. Recompute liveness in that final namespace and remove pure values
	// whose destination is overwritten or dead on every successor before it is read.
	static void eliminate_dead_colored_moves(mprocedure* proc) {
		bool removed;
		do {
			msize_t max_reg_id = 0;
			for (auto& block : proc->basic_blocks) {
				for (const auto& instruction : block.instructions) {
					instruction.for_each_reg_w_implicit([&](mreg reg, bool) {
						if (!is_pseudo(reg))
							max_reg_id = std::max(max_reg_id, reg.uid() + 1);
					});
				}
			}

			for (auto& block : proc->basic_blocks) {
				block.df_def.clear();
				block.df_ref.clear();
				block.df_in_live.clear();
				block.df_out_live.clear();
				block.df_def.resize(max_reg_id);
				block.df_ref.resize(max_reg_id);
				block.df_in_live.resize(max_reg_id);
				block.df_out_live.resize(max_reg_id);
				for (const auto& instruction : block.instructions) {
					instruction.for_each_reg_w_implicit([&](mreg reg, bool read) {
						if (is_pseudo(reg))
							return;
						if (read) {
							if (!block.df_def[reg.uid()])
								block.df_ref.set(reg.uid());
						} else {
							block.df_def.set(reg.uid());
						}
					});
				}
			}

			bool liveness_changed;
			do {
				liveness_changed = false;
				for (auto& block : view::reverse(proc->basic_blocks)) {
					util::bitset out{max_reg_id};
					for (auto* successor : block.successors)
						out.set_union(successor->df_in_live);
					auto in = out;
					in.set_difference(block.df_def);
					in.set_union(block.df_ref);
					if (out != block.df_out_live) {
						out.swap(block.df_out_live);
						liveness_changed = true;
					}
					if (in != block.df_in_live) {
						in.swap(block.df_in_live);
						liveness_changed = true;
					}
				}
			} while (liveness_changed);

			removed = false;
			for (auto& block : proc->basic_blocks) {
				auto live = block.df_out_live;
				for (size_t index = block.instructions.size(); index-- != 0;) {
					auto& instruction     = block.instructions[index];
					bool  may_trap        = instruction.is(vop::idiv) || instruction.is(vop::iudiv) || instruction.is(vop::imod);
					bool  writes_implicit = instruction.effects.implicit_gp_write || instruction.effects.implicit_fp_write;
					bool  removable       = instruction.out && instruction.out.is_phys() && !instruction.has_side_effects() && !may_trap && !writes_implicit;
					if (removable && !live[instruction.out.uid()]) {
						block.instructions.erase(block.instructions.begin() + index);
						removed = true;
						continue;
					}
					instruction.for_each_reg_w_implicit([&](mreg reg, bool read) {
						if (is_pseudo(reg))
							return;
						if (!read)
							live.reset(reg.uid());
					});
					instruction.for_each_reg_w_implicit([&](mreg reg, bool read) {
						if (read && !is_pseudo(reg))
							live.set(reg.uid());
					});
				}
			}
		} while (removed);
	}

	static void order_cold_blocks(mprocedure* proc) {
		// Keep failure/cleanup regions out of the hot fallthrough order. list::splice
		// preserves every mblock address referenced by CFG edges.
		std::list<mblock> cold;
		for (auto it = std::next(proc->basic_blocks.begin()); it != proc->basic_blocks.end();) {
			auto current = it++;
			if (current->hot < 0)
				cold.splice(cold.end(), proc->basic_blocks, current);
		}
		proc->basic_blocks.splice(proc->basic_blocks.end(), cold);
	}

	// Dense virtual IDs bound the size of block liveness sets after dead IR
	// values have disappeared during lowering. Physical and pseudo IDs stay fixed.
	static void compact_virtual_registers(mprocedure* proc) {
		std::unordered_map<int32_t, int32_t> renamed;
		int32_t                              next_gp = vreg_first;
		int32_t                              next_fp = 0;
		auto                                 rename  = [&](const mreg& r) {
			if (!r.is_virt() || is_pseudo(r))
				return;
			auto [it, inserted] = renamed.try_emplace(r.id, 0);
			if (inserted)
				it->second = r.is_fp() ? -++next_fp : next_gp++;
			const_cast<mreg&>(r).id = it->second;
		};
		for (auto& bb : proc->basic_blocks) {
			for (auto& i : bb.instructions)
				i.for_each_reg([&](const mreg& r, bool) { rename(r); });
		}
		proc->next_reg_i = next_gp - vreg_first;
		proc->next_reg_f = next_fp;
	}

	// Allocates registers for each virtual register and generates the spill instructions.
	//
	void allocate_registers(mprocedure* proc) {
		compact_virtual_registers(proc);
		order_cold_blocks(proc);
		normalize_tied_constraints(proc);

		// Spill arguments.
		//
		spill_args(proc);

		// Build the interference graph.
		//
		std::vector<graph_node> interference_graph = build_graph(proc);

		// Enter the register allocation loop.
		//
	#if LI_RA_TEST_PRESSURE
		static constexpr size_t MAX_K = 4;
		static constexpr size_t MAX_M = 3;
	#else
		static constexpr size_t MAX_K = arch::num_gp_reg;
		static constexpr size_t MAX_M = arch::num_fp_reg;
	#endif
		size_t K               = std::min(MAX_K, std::max<size_t>(std::size(arch::gp_volatile), 2));
		size_t M               = std::min(MAX_M, std::max<size_t>(std::size(arch::fp_volatile), 2));
		auto   allocation_mask = [](size_t count, const auto& volatile_regs, const auto& nonvolatile_regs) {
			uint64_t result = 0;
			auto     append = [&](const auto& registers) {
				for (arch::reg r : registers) {
					if (!count)
						break;
					result |= uint64_t{1} << (std::abs(int32_t(r)) - 1);
					count--;
				}
			};
			append(volatile_regs);
			append(nonvolatile_regs);
			return result;
		};
		// try_color rewrites only the colors and spill markers it owns and resets
		// them on entry, so retrying with more registers needs no graph snapshot.
		size_t spill_budget = interference_graph.size();

		// Preserve conflicts with sources spilled in earlier rewrite rounds. The
		// rewritten graph no longer contains those virtual live ranges, so without
		// this history every round permanently appends a fresh set of stack slots.
		std::vector<std::vector<int32_t>> forbidden_spill_slots(interference_graph.size());
		int32_t                           max_spill_slot = 0;

		int32_t base_stack_slots = (proc->used_stack_length + 7) / 8;
		int32_t num_spill_slots  = base_stack_slots;
		proc->spill_slots        = 0;
		for (;;) {
			// Try coloring the graph.
			//
			uint64_t gp_colors        = allocation_mask(K, arch::gp_volatile, arch::gp_nonvolatile);
			uint64_t fp_colors        = allocation_mask(M, arch::fp_volatile, arch::fp_nonvolatile);
			auto [spill_gp, spill_fp] = try_color(interference_graph, gp_colors, fp_colors);
			// printf("Try_color (K=%llu, M=%llu) spills (%llu, %llu) registers\n", K, M, spill_gp, spill_fp);

			// If we don't need to spill, break out.
			//
			if (!spill_gp && !spill_fp) {
				break;
			}

			// If we have more registers to allocate, restore old graph and try again.
			//
			bool increase_k = spill_gp && K != MAX_K;
			bool increase_m = spill_fp && M != MAX_M;
			if (increase_k)
				K = spill_gp > MAX_K - K ? MAX_K : K + 1;
			if (increase_m)
				M = spill_fp > MAX_M - M ? MAX_M : M + 1;
			if (increase_k || increase_m)
				continue;

			// Each rewrite eliminates at least one finite-priority source and
			// introduces only constrained reload/store temporaries. This gives
			// spill retries a deterministic bound without rejecting large valid
			// procedures at an arbitrary iteration count.
			//
			size_t spilled_sources = 0;
			for (auto& node : interference_graph) {
				if (node.spill_slot && node.priority != std::numeric_limits<float>::infinity())
					spilled_sources++;
			}
			if (!spilled_sources || spilled_sources > spill_budget)
				util::abort("register allocation spill retry made no progress");
			spill_budget -= spilled_sources;

			// Recolor all spills against slots retained from previous rewrite rounds.
			// Largest-pressure-first gives first-fit a stable, compact ordering.
			std::vector<size_t> current_spills;
			current_spills.reserve(spilled_sources);
			for (size_t i = 0; i != interference_graph.size(); ++i) {
				if (interference_graph[i].spill_slot)
					current_spills.emplace_back(i);
			}
			std::sort(current_spills.begin(), current_spills.end(), [&](size_t a, size_t b) {
				auto a_pressure = interference_graph[a].neighbors.size();
				auto b_pressure = interference_graph[b].neighbors.size();
				return a_pressure != b_pressure ? a_pressure > b_pressure : a < b;
			});
			for (size_t idx : current_spills) {
				auto& node      = interference_graph[idx];
				node.spill_slot = 1;
				while (true) {
					bool conflict =
						 std::find(forbidden_spill_slots[idx].begin(), forbidden_spill_slots[idx].end(), node.spill_slot) != forbidden_spill_slots[idx].end();
					for (size_t other : current_spills) {
						if (other == idx || interference_graph[other].spill_slot <= 0 || interference_graph[other].spill_slot != node.spill_slot)
							continue;
						const auto& neighbors = interference_graph[other].neighbors;
						conflict              = interference_graph[other].is_fp != node.is_fp || std::binary_search(neighbors.begin(), neighbors.end(), idx);
						if (conflict)
							break;
					}
					if (!conflict)
						break;
					node.spill_slot++;
				}
				max_spill_slot = std::max(max_spill_slot, node.spill_slot);
			}
			num_spill_slots = std::max(num_spill_slots, base_stack_slots + max_spill_slot);

			// Record which of these slots overlap each source that may spill later.
			// Register classes have no cross-class interference edges, so keep their
			// slots distinct conservatively.
			for (size_t i = 0; i != interference_graph.size(); ++i) {
				auto& node = interference_graph[i];
				if (!node.present || node.spill_slot || node.priority == std::numeric_limits<float>::infinity())
					continue;
				for (size_t spilled : current_spills) {
					auto& prior = interference_graph[spilled];
					if (node.is_fp != prior.is_fp || std::binary_search(node.neighbors.begin(), node.neighbors.end(), spilled)) {
						auto& forbidden = forbidden_spill_slots[i];
						if (std::find(forbidden.begin(), forbidden.end(), prior.spill_slot) == forbidden.end())
							forbidden.emplace_back(prior.spill_slot);
					}
				}
			}

			// Add spilling code.
			//
			struct spill_entry {
				mreg    src    = {};
				mreg    dst    = {};
				int32_t slot   = 0;
				bool    reload = false;
				bool    store  = false;
			};
			int32_t slot_offset = base_stack_slots;
			bool    spilled_any = false;
			for (auto& bb : proc->basic_blocks) {
				for (auto it = bb.instructions.begin(); it != bb.instructions.end();) {
					// Three operands may each contain a base and index register, plus
					// the output. Duplicate reads and read/write operands share one
					// temporary so destructive instructions remain valid.
					//
					spill_entry spill_entries[7] = {};
					bool        need_cg          = false;

					auto spill_and_swap = [&](mreg& m, bool is_read, int32_t slot) {
						need_cg     = true;
						spilled_any = true;
						for (auto& entry : spill_entries) {
							if (entry.src == m) {
								entry.reload |= is_read;
								entry.store |= !is_read;
								m = entry.dst;
								return;
							}
							if (entry.src.is_null()) {
								entry.src       = m;
								entry.dst       = m.is_fp() ? proc->next_fp() : proc->next_gp();
								entry.slot      = slot + slot_offset - 1;
								entry.reload    = is_read;
								entry.store     = !is_read;
								m               = entry.dst;
								num_spill_slots = std::max(num_spill_slots, entry.slot + 1);
								return;
							}
						}
						assume_unreachable();
					};
					it->for_each_reg([&](const mreg& r, bool is_read) {
						if (is_pseudo(r) || !r.is_virt())
							return;
						if (interference_graph.size() <= r.uid())
							return;

						// Skip if not spilled.
						//
						auto& info = interference_graph[r.uid()];
						if (!info.spill_slot) {
							return;
						}
						spill_and_swap(const_cast<mreg&>(r), is_read, info.spill_slot);
					});

					// If we don't need to change anything, continue.
					//
					if (!need_cg) {
						++it;
						continue;
					}

					// Reload and spill as requested.
					//
					for (auto& entry : spill_entries) {
						if (entry.src.is_null())
							break;
						if (!entry.reload)
							continue;
						auto  op = entry.src.is_fp() ? vop::loadf64 : vop::loadi64;
						mmem  mem{.base = arch::sp, .disp = entry.slot * 8};
						minsn i{op, entry.dst, mem};
						i.no_spill = true;
						it         = bb.instructions.insert(it, i) + 1;
					}

					auto next = it + 1;
					for (auto& entry : spill_entries) {
						if (entry.src.is_null())
							break;
						if (!entry.store)
							continue;
						auto  op = entry.src.is_fp() ? vop::storef64 : vop::storei64;
						mmem  mem{.base = arch::sp, .disp = entry.slot * 8};
						minsn i{op, {}, mem, entry.dst};
						i.no_spill = true;
						next       = bb.instructions.insert(next, i) + 1;
					}
					it = next;
				}
			}
			LI_ASSERT(spilled_any);

			// Rebuild the interference graph.
			//
			interference_graph = build_graph(proc, &interference_graph);
			forbidden_spill_slots.resize(interference_graph.size());
		}
		proc->used_stack_length = num_spill_slots * 8;
		proc->spill_slots       = uint32_t(num_spill_slots - base_stack_slots);

		// Swap the registers in the IR. Constrained/precolored registers did not
		// pass through coloring, but they are already physical operands.
		//
		for (auto& bb : proc->basic_blocks) {
			for (auto& i : bb.instructions) {
				i.for_each_reg([&](const mreg& r, bool) {
					if (is_pseudo(r) || r.is_phys())
						return;
					int color = int(interference_graph[r.uid()].color);
					LI_ASSERT(color != 0);
					const_cast<mreg&>(r) = arch::reg(r.is_fp() ? -color : color);
				});
			}
		}

		eliminate_dead_colored_moves(proc);

		// Derive save masks only from instructions that survived final move DCE.
		proc->used_gp_mask = 0;
		proc->used_fp_mask = 0;
		for (auto& bb : proc->basic_blocks) {
			for (auto& i : bb.instructions) {
				i.for_each_reg([&](const mreg& r, bool) {
					if (is_pseudo(r))
						return;
					LI_ASSERT(r.is_phys());
					if (r.is_fp())
						proc->used_fp_mask |= arch::reg_mask(r.phys());
					else
						proc->used_gp_mask |= arch::reg_mask(r.phys());
				});
			}
		}
		// Remove eliminated moves.
		//
		for (auto& bb : proc->basic_blocks) {
			std::erase_if(bb.instructions, [](minsn& i) {
				if (i.is(vop::movf) || i.is(vop::movi)) {
					if (i.arg[0].is_reg())
						return i.out == i.arg[0].reg;
				}
				return false;
			});
		}
	}
};

#endif