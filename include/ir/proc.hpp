#pragma once
#include <ir/insn.hpp>
#include <list>
#include <string>
#include <vector>
#include <unordered_map>
#include <vm/bc.hpp>
#include <vm/function.hpp>

namespace li::ir {
	// Basic block type.
	//
	struct basic_block {
		// Unique identifier, note that this may change on sort.
		//
		uint32_t uid = 0;

		// Details of the basic block itself.
		//
		procedure* proc      = nullptr;  // Procedure it belongs to.
		uint8_t    cold_hint = 0;        // Number specifying how cold this block is.

		// Bytecode ranges.
		//
		bc::pos bc_begin = 0;
		bc::pos bc_end   = 0;

		// Successor and predecesor list.
		//
		std::vector<basic_block*> successors  = {};
		std::vector<basic_block*> predecesors = {};

		// Instruction list.
		//
		std::list<insn_ref> instructions = {};  // TODO: lame

		// Temporary for search algorithms.
		//
		bool visited = false;

		// Replace all uses of a value in this block.
		//
		size_t replace_all_uses(value* of, const std::shared_ptr<value>& with) {
			size_t n = 0;
			for (auto& i : instructions) {
				for (auto& op : i->operands) {
					if (op.get() == of) {
						op = with;
						n++;
					}
				}
			}
			return n;
		}

		// Validates the basic block.
		//
		void validate() {
#if LI_DEBUG
			size_t num_term = 0;
			bool   phi_ok   = true;
			for (auto it = instructions.begin(); it != instructions.end(); ++it) {
				auto& i = *it;

				if (i->op != opcode::phi) {
					phi_ok = false;
				} else if (!phi_ok) {
					util::abort("phi used after block header.");
				}
				num_term += i->is_terminator();

				for (auto& op : i->operands) {
					if (!op->is<insn>())
						continue;
					if (op->get<insn>()->parent == this) {
						auto it2 = std::find(instructions.begin(), it, op);
						if (it2 == it) {
							util::abort("cyclic reference found: %s", i->to_string(true).c_str());
						}
					} else if (!op->get<insn>()->parent) {
						util::abort("dangling reference found: %s", i->to_string(true).c_str());
					}
				}
				i->update();
			}

			if (num_term == 0) {
				util::abort("block is not terminated");
			}
			if (num_term > 1) {
				util::abort("block has multiple terminators");
			}
#endif
		}

		// Domination check.
		//
		bool dom(basic_block* n);
		bool postdom(basic_block* n);

		// No copy, construction with procedure reference.
		//
		basic_block(procedure* proc) : proc(proc) {}
		basic_block(const basic_block&)            = delete;
		basic_block& operator=(const basic_block&) = delete;

		// Kill parent reference of all instructions on destruction.
		//
		~basic_block() {
			for (auto& i : instructions) {
				i->parent = nullptr;
			}
		}
	};
	static std::string to_string(basic_block* bb) { return util::fmt(LI_PRP "$%u" LI_DEF, bb->uid); }

	// Procedure type.
	//
	struct procedure {
		// Source information.
		//
		vm*       L = nullptr;  // Related VM and function.
		function* f = nullptr;  //

		// Constant pool.
		//
		struct const_hash {
			size_t operator()(const constant& c) const {
				return std::hash<int64_t>{}(c.i) ^ (size_t)c.vt;
			}
		};
		std::unordered_map<constant, std::shared_ptr<constant>, const_hash> consts = {};

		// Procedure state.
		//
		std::vector<std::unique_ptr<basic_block>> basic_blocks;        // List of basic blocks, first is entry point.
		uint32_t                                  next_reg_name  = 0;  // Next register name.
		uint32_t                                  next_block_uid = 0;  // Next block uid.

		// Cached analysis flags.
		//
		uint32_t is_topologically_sorted : 1 = 0;

		// Constructed by VM instance and the function we're translating.
		//
		procedure(vm* L, function* f) : L(L), f(f) {}

		// Gets the entry point.
		//
		basic_block* get_entry() { return basic_blocks.empty() ? nullptr : basic_blocks.front().get(); }

		// Creates a new block.
		//
		basic_block* add_block() {
			is_topologically_sorted = false;
			auto* blk = basic_blocks.emplace_back(std::make_unique<basic_block>(this)).get();
			blk->uid  = next_block_uid++;
			return blk;
		}
		auto del_block(basic_block* b) {
			auto it = consts.find(constant(b));
			LI_ASSERT(it->second.use_count() == 1);
			consts.erase(it);
			LI_ASSERT(b->predecesors.empty());
			LI_ASSERT(b->successors.empty());
			for (auto it = basic_blocks.begin();; ++it) {
				LI_ASSERT(it != basic_blocks.end());
				if (it->get() == b) {
					return basic_blocks.erase(it);
				}
			}
		}

		// Adds or deletes a jump.
		//
		void add_jump(basic_block* from, basic_block* to) {
			if (auto it = std::find(from->successors.begin(), from->successors.end(), to); it == from->successors.end()) {
				from->successors.emplace_back(to);
				is_topologically_sorted = false;
			}
			if (auto it = std::find(to->predecesors.begin(), to->predecesors.end(), from); it == to->predecesors.end()) {
				to->predecesors.emplace_back(from);
				is_topologically_sorted = false;
			}
		}
		void del_jump(basic_block* from, basic_block* to) {
			if (auto it = std::find(from->successors.begin(), from->successors.end(), to); it != from->successors.end()) {
				from->successors.erase(it);
				is_topologically_sorted = false;
			}
			if (auto it = std::find(to->predecesors.begin(), to->predecesors.end(), from); it != to->predecesors.end()) {
				to->predecesors.erase(it);
				is_topologically_sorted = false;
			}
		}

		// Templated DFS/BFS helper.
		//
		template<typename F>
		void dfs(F&& fn, basic_block* from = nullptr) {
			for (auto& b : basic_blocks) {
				b->visited = false;
			}
			auto rec = [&](auto& self, basic_block* b) -> void {
            b->visited = true;
            for (auto& s : b->successors)
               if (!s->visited)
                  self(self, s);
				fn(b);
			};
			rec(rec, from ? from : get_entry());
		}
		template<typename F>
		void bfs(F&& fn, basic_block* from = nullptr) {
			for (auto& b : basic_blocks) {
				b->visited = false;
			}
			auto rec = [&](auto& self, basic_block* b) -> void {
				b->visited = true;
				fn(b);
				for (auto& s : b->successors)
					if (!s->visited)
						self(self, s);
			};
			rec(rec, from ? from : get_entry());
		}
		template<typename F>
		void rdfs(F&& fn, basic_block* from) {
			for (auto& b : basic_blocks) {
				b->visited = false;
			}
			auto rec = [&](auto& self, basic_block* b) -> void {
				b->visited = true;
				for (auto& s : b->predecesors)
					if (!s->visited)
						self(self, s);
				fn(b);
			};
			rec(rec, from);
		}
		template<typename F>
		void rbfs(F&& fn, basic_block* from) {
			for (auto& b : basic_blocks) {
				b->visited = false;
			}
			auto rec = [&](auto& self, basic_block* b) -> void {
				b->visited = true;
				fn(b);
				for (auto& s : b->predecesors)
					if (!s->visited)
						self(self, s);
			};
			rec(rec, from);
		}

		// Topologically sorts the basic block list.
		//
		void toplogical_sort() {
			if (is_topologically_sorted)
				return;
			uint32_t tmp = (uint32_t) basic_blocks.size();
			dfs([&](basic_block* b) { b->uid = --tmp; });
			LI_ASSERT(get_entry()->uid == 0);
			std::sort(basic_blocks.begin(), basic_blocks.end(), [](auto& a, auto& b) { return a->uid < b->uid; });
			is_topologically_sorted = true;
		}

		// Renames all registers and blocks after topologically sorting.
		//
		void reset_names() {
			toplogical_sort();
			next_reg_name = 0;
			for (auto& bb : basic_blocks) {
				for (auto& i : bb->instructions) {
					i->name = next_reg_name++;
				}
			}
		}

		// Replace all uses of a value throughout the entire procedure.
		//
		size_t replace_all_uses(value* of, const std::shared_ptr<value>& with) {
			size_t n = 0;
			for (auto& b : basic_blocks) {
				n += b->replace_all_uses(of, with);
			}
			return n;
		}

		// Validates all basic blocks.
		//
		void validate() {
#if LI_DEBUG
			LI_ASSERT(get_entry() != nullptr);
			for (auto& i : basic_blocks)
				i->validate();
#endif
		}

		// Printer.
		//
		void print() {
			printf("---------------------------------\n");
			for (auto& bb : basic_blocks) {
				printf("--- Block %u %s\n", bb->uid, bb->cold_hint ? "[COLD]" : "");
				for (auto& i : bb->instructions) {
					printf(LI_GRN "#%-5x" LI_DEF " %s\n", i->source_bc, i->to_string(true).c_str());
				}
			}
		}
	};

	// Handles constants.
	//
	template<typename Tv>
	static value_ref launder_value(procedure* proc, Tv&& v) {
		if constexpr (!std::is_convertible_v<Tv, value_ref>) {
			constant c{std::forward<Tv>(v)};
			auto     it = proc->consts.find(c);
			if (it == proc->consts.end()) {
				it = proc->consts.emplace(c, std::make_shared<constant>(c)).first;
			}
			return it->second;
		} else {
			return value_ref(v);
		}
	}

	// Builder context.
	//
	struct builder {
		basic_block* blk        = nullptr;
		bc::pos      current_bc = bc::no_pos;
		builder(basic_block* b) : blk(b) {}

		// Instruction emitting.
		//
		template<typename T, typename... Tx>
		insn_ref create(Tx&&... args) {
			// Set basic details.
			//
			std::shared_ptr<T> i = std::make_shared<T>();
			i->parent            = blk;
			i->name              = blk->proc->next_reg_name++;
			i->source_bc         = current_bc;
			i->op                = T::Opcode;
			i->vt                = type::unk;

			// Add operands and update.
			//
			i->operands = std::vector<value_ref>{launder_value(blk->proc, std::forward<Tx>(args))...};
			i->update();
			return i;
		}
		template<typename T, typename... Tx>
		insn_ref emit(Tx&&... args) {
			auto i = create<T, Tx...>(std::forward<Tx>(args)...);
			if (i->source_bc == bc::no_pos) {
				i->source_bc = blk->bc_end;
			}
			blk->instructions.emplace_back(i);
			return i;
		}
		template<typename T, typename... Tx>
		insn_ref emit_front(Tx&&... args) {
			auto i = create<T, Tx...>(std::forward<Tx>(args)...);
			if (i->source_bc == bc::no_pos) {
				i->source_bc = blk->bc_begin;
			}
			blk->instructions.emplace_front(i);
			return i;
		}
	};
};