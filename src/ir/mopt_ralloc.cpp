#include <ir/opt.hpp>

/*

A) instruction traits [encoding, etc]
B) reg alloc


-> type & trait inference
---> register allocation
1) flags register
2) can't spill or re-type conditionally
- optimize type inference with dominator trees


-> register allocation

*/

namespace li::ir::opt {
	struct graph_node {
		util::bitset vtx   = {};
		uint8_t      color = {};
	};

	// Returns a view that can be enumerated as a series of mregs for a given bitset.
	//
	static auto regs_in(const util::bitset& bs) {
		return view::iota(0ull, bs.size()) | view::filter([&](size_t n) { return bs[n]; }) | view::transform([&](size_t n) { return mreg::from_uid(uint32_t(n)); });
	}

	// Returns true if register should be included in the interference graph.
	//
	static bool interferes_with(mreg a, mreg b) {
		// Ignore certain registers.
		//
		if (a.is_flag() || b.is_flag())
			return false;
		if ((a.is_virt() && a.virt() == vreg_cpool) || (b.is_virt() && b.virt() == vreg_cpool))
			return false;

		// Match target class.
		//
		if (a.is_fp() != b.is_fp())
			return false;
		return true;
	}

	// Tries coloring the graph with K colors.
	//
	static bool try_color(std::span<graph_node> gr, size_t K) {
		// Pick a node we can simplify with < K nodes.
		//
		bool found_but_over_limit = false;
		auto it = range::find_if(gr, [&](graph_node& n) {
			// Pre-colored.
			//
			if (n.color)
				return false;

			// Disconnected or over the limit.
			//
			size_t deg = n.vtx.popcount();
			if (deg == 0)
				return false;
         found_but_over_limit = true;
         if (deg > K /*>=K but we also set one for ourselves so yeah*/) {
            return false;
			}
			return true;
		});

		// If we did not find any, fail unless there really isnt any nodes left.
		//
		if (it == gr.end()) {
			return !found_but_over_limit;
		}

		// Remove from the graph.
		//
		util::bitset tmp{gr.size()};
		tmp.swap(it->vtx);
		for (size_t i = 0; i != gr.size(); i++) {
			if (tmp[i])
				gr[i].vtx.reset(it - gr.begin());
		}

		// Recursively color.
		//
		bool rec_result = try_color(gr, K);

		// Add the node back.
		//
		size_t color_mask = ~0ull;
		for (size_t i = 0; i != gr.size(); i++) {
			if (tmp[i]) {
				gr[i].vtx.set(it - gr.begin());
				if (gr[i].color != 0) {
					color_mask &= ~(1ull << (gr[i].color - 1));
				}
			}
		}
		tmp.swap(it->vtx);

		// If recursive call failed, propagate.
		//
		if (!rec_result)
			return false;

		// Pick a color.
		//
		size_t n = std::countr_zero(color_mask);
		if (n > K)
			return false;
		it->color = n + 1;
		return true;
	}
	
	// Allocates registers for each virtual register and generates the spill instructions.
	//
	void allocate_registers(mprocedure* proc) {
		// Before anything else, spill all arguments into virtual registers.
		//
		mreg regs[3] = {};
		for (auto& bb : proc->basic_blocks) {
			for (auto& i : bb.instructions) {
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
				proc->basic_blocks.front().instructions.insert(proc->basic_blocks.front().instructions.begin(), minsn{vop::movi, regs[i], mreg(arch::map_argument(i, 0, false))});
			}
		}

		// Get maximum register id.
		//
		uint32_t max_reg_id = 0;
		for (auto& bb : proc->basic_blocks) {
			for (auto& i : bb.instructions) {
				i.for_each_reg([&](mreg r, bool is_read) {
					max_reg_id = std::max(r.uid(), max_reg_id);
				});
			}
		}
		++max_reg_id;

		// First calculate ref(n) and def(n) for each basic block.
		//
		for (auto& bb : proc->basic_blocks) {
			bb.df_def.resize(max_reg_id);
			bb.df_ref.resize(max_reg_id);
			bb.df_live.resize(max_reg_id);

			for (auto& i : bb.instructions) {
				i.for_each_reg([&](mreg r, bool is_read) {
					if (is_read) {
						if (!bb.df_def[r.uid()]) {
							bb.df_ref.set(r.uid());
						}
					} else {
						bb.df_def.set(r.uid());
					}
				});
			}
		}

		// Calculate live ranges:
		// - in-live(n) = (out-live(n)\def(n)) U ref(n)
		// - out-live(n) = for each succ, (... U in-live(s))
		//
		bool changed;
		do {
			changed = false;

			// For each block:
			//
			for (auto& bb : proc->basic_blocks) {
				util::bitset new_live{max_reg_id};
				for (auto& s : bb.successors) {
					new_live.set_union(s->df_live);
				}
				new_live.set_difference(bb.df_def);
				new_live.set_union(bb.df_ref);
				if (new_live != bb.df_live) {
					changed = true;
					new_live.swap(bb.df_live);
				}
			}
		} while (changed);

		//for (auto& b : proc->basic_blocks) {
		//	printf("-- Block $%u", b.uid);
		//	if (b.hot < 0)
		//		printf(LI_CYN " [COLD %u]" LI_DEF, (uint32_t) -b.hot);
		//	if (b.hot > 0)
		//		printf(LI_RED " [HOT  %u]" LI_DEF, (uint32_t) b.hot);
		//	putchar('\n');
		//
		//	printf("Live = ");
		//	for (mreg r : regs_in(b.df_live))
		//		printf(" %s", r.to_string().c_str());
		//	printf("\n");
		//	printf("Def = ");
		//	for (mreg r : regs_in(b.df_def))
		//		printf(" %s", r.to_string().c_str());
		//	printf("\n");
		//	printf("Ref = ");
		//	for (mreg r : regs_in(b.df_ref))
		//		printf(" %s", r.to_string().c_str());
		//	printf("\n");
		//
		//	b.print();
		//}

		// Allocate the interference graph.
		//
		auto graph_alloc = std::make_unique<graph_node[]>(max_reg_id);
		std::span interference_graph{&graph_alloc[0], max_reg_id};

		{
			// Reset all nodes.
			//
			for (auto& node : interference_graph) {
				node.color = 0;
				node.vtx.clear();
				node.vtx.resize(max_reg_id);
			}

			// Build the interference graph.
			//
			auto add_vertex = [&](mreg a, mreg b) {
				if (!interferes_with(a, b))
					return true;
				auto au   = a.uid();
				auto bu   = b.uid();
				bool prev = interference_graph[au].vtx.set(bu);
				interference_graph[bu].vtx.set(au);
				return prev;
			};
			auto add_set = [&](const util::bitset& b, mreg def) {
				interference_graph[def.uid()].vtx.set(def.uid());
				if (def.is_phys()) {
					interference_graph[def.uid()].color = std::abs(def.phys());
				} else if (def.is_virt() && def == vreg_vm) {
					interference_graph[def.uid()].color = arch::map_argument(0, 0, false);
				} else if (def.is_virt() && def == vreg_args) {
					interference_graph[def.uid()].color = arch::map_argument(1, 0, false);
				} else if (def.is_virt() && def == vreg_nargs) {
					interference_graph[def.uid()].color = arch::map_argument(2, 0, false);
				}
				for (size_t i = 0; i != max_reg_id; i++) {
					if (b[i]) {
						add_vertex(def, mreg::from_uid(i));
					}
				}
			};

			for (auto& b : proc->basic_blocks) {
				auto live = b.df_live;
				for (auto& i : view::reverse(b.instructions)) {

					if (i.out)
						live.reset(i.out.uid());
					i.for_each_reg([&](mreg r, bool is_read) {
						if (is_read) {
							live.set(r.uid());
						}
					});

					i.for_each_reg([&](mreg r, bool is_read) {
						if (is_read) {
							add_set(live, r);
						}
					});
				}
			}

			// Try each K until we get to succeed.
			//
			size_t num_vol  = std::min(std::size(arch::gp_volatile), std::size(arch::fp_volatile));
			size_t max_regs = std::min(arch::num_fp_reg, arch::num_gp_reg);
			for (size_t K = num_vol;; K++) {
				bool ok = try_color(interference_graph, K);
				printf("Try_color K=%llu -> %s\n", K, ok ? "OK" : "Fail");
				if (ok)
					break;
				LI_ASSERT(K != max_regs); // TODO: Spill
			}

			// Draw the graph.
			//
			//printf("graph {\n node [colorscheme=set312 penwidth=5]\n");
			//for (size_t i = 0; i != max_reg_id; i++) {
			//	for (size_t j = 0; j != max_reg_id; j++) {
			//		if (interference_graph[i].vtx.reset(j)) {
			//			interference_graph[j].vtx.reset(i);
			//
			//			if (i != j) {
			//				auto v = mreg::from_uid(i);
			//				auto k = mreg::from_uid(j);
			//
			//				printf("r%u [color=%u label=\"%s\"];\n", v.uid(), interference_graph[i].color, v.to_string().c_str());
			//				printf("r%u [color=%u label=\"%s\"];\n", k.uid(), interference_graph[j].color, k.to_string().c_str());
			//				printf("r%u -- r%u;\n", k.uid(), v.uid());
			//			}
			//		}
			//	}
			//}
			//printf("}\n");

			// Swap the registers in the IR.
			//
			for (auto& bb : proc->basic_blocks) {
				for (auto& i : bb.instructions) {
					i.for_each_reg([&](const mreg& r, bool is_read) {
						if (!r.is_phys() && !r.is_flag() && (!r.is_virt() || r.virt() != vreg_cpool)) {
							int x = int(interference_graph[r.uid()].color);
							if (r.is_fp())
								x = -x;
							if (!x)
								util::abort("Missed allocation for %s (%u)?", r.to_string().c_str(), r.uid());
							const_cast<mreg&>(r) = arch::reg(x);
						}
						return false;
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
	}
};