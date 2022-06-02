#pragma once
#include <ir/insn.hpp>
#include <list>
#include <string>
#include <unordered_map>
#include <util/llist.hpp>
#include <vector>
#include <vm/bc.hpp>
#include <vm/function.hpp>

namespace li::ir {
	struct instruction_iterator {
		using iterator_category = std::bidirectional_iterator_tag;
		using difference_type   = ptrdiff_t;
		using value_type        = insn*;
		using pointer           = insn*;
		using reference         = insn*;

		insn* at;
		instruction_iterator(insn* at) : at(at) {}

		reference             operator*() const { return at; }
		pointer               operator->() { return at; }
		instruction_iterator& operator++() {
			at = at->next;
			return *this;
		}
		instruction_iterator& operator--() {
			at = at->prev;
			return *this;
		}
		instruction_iterator operator++(int) {
			instruction_iterator tmp = *this;
			++*this;
			return tmp;
		}
		instruction_iterator operator--(int) {
			instruction_iterator tmp = *this;
			++*this;
			return tmp;
		}
		bool operator==(const instruction_iterator& o) const { return at == o.at; };
		bool operator!=(const instruction_iterator& o) const { return at != o.at; };
	};

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
		mutable insn insn_list_head = {};

		// Temporary for search algorithms.
		//
		mutable bool visited = false;

		// Container observers.
		//
		instruction_iterator begin() const { return {insn_list_head.next}; }
		instruction_iterator end() const { return {&insn_list_head}; }
		auto                 rbegin() const { return std::reverse_iterator(end()); }
		auto                 rend() const { return std::reverse_iterator(begin()); }
		bool                 empty() const { return insn_list_head.next == &insn_list_head; }
		ref<insn>            front() const { return (!empty()) ? make_ref(insn_list_head.next) : nullptr; }
		ref<insn>            back() const { return (!empty()) ? make_ref(insn_list_head.prev) : nullptr; }

		// Insertion.
		//
		instruction_iterator insert(instruction_iterator position, ref<insn> v) {
			LI_ASSERT(!v->parent);
			v->parent = this;
			util::link_before(position.at, v.get());
			return {v.release()};
		}
		instruction_iterator push_front(ref<insn> v) { return insert(begin(), std::move(v)); }
		instruction_iterator push_back(ref<insn> v) { return insert(end(), std::move(v)); }

		// Erase wrapper.
		//
		instruction_iterator erase(instruction_iterator it) {
			auto next = it->next;
			it->erase();
			return next;
		}
		instruction_iterator erase(ref<insn> it) {
			auto r = erase(instruction_iterator(it.get()));
			it.reset();
			return r;
		}
		template<typename F>
		size_t erase_if(F&& f) {
			size_t n = 0;
			for (auto it = begin(); it != end();) {
				if (f(it.at)) {
					n++;
					it = erase(it);
				} else {
					++it;
				}
			}
			return n;
		}

		// Splits the basic block at instruction boundary and adds a jump to the next block after it.
		//
		basic_block* split_at(const insn* at);

		// Validates the basic block.
		//
		void validate() {
#if LI_DEBUG
			size_t num_term = 0;
			bool   phi_ok   = true;
			for (auto i : *this) {
				for (auto& op : i->operands) {
					if (!op->is<insn>())
						continue;
					if (op->as<insn>()->parent == this) {
						auto it2 = std::find(begin(), instruction_iterator(i), op);
						if (it2 == instruction_iterator(i)) {
							util::abort("cyclic reference found: %s", i->to_string(true).c_str());
						}
					} else if (!op->as<insn>()->parent) {
						util::abort("dangling reference found: %s", i->to_string(true).c_str());
					}
				}

				if (!i->is<phi>()) {
					phi_ok = false;
				} else if (!phi_ok) {
					util::abort("phi used after block header.");
				} else {
					LI_ASSERT(i->operands.size() == predecesors.size());
					for (size_t j = 0; j != i->operands.size(); j++) {
						if (i->operands[j]->is<insn>()) {
							LI_ASSERT(i->operands[j]->as<insn>()->parent->dom(predecesors[j]));
						}
					}
				}
				num_term += i->is_terminator();
				if (i->is_proc_terminator()) {
					LI_ASSERT(successors.empty());
				}

				i->update();
			}

			if (num_term == 0) {
				util::abort("block is not terminated");
			}
			if (num_term > 1) {
				util::abort("block has multiple terminators");
			}
#else
			for (auto it = instructions.begin(); it != instructions.end(); ++it) {
				it->get()->update();
			}
#endif
		}

		// Printer.
		//
		void print() const {
			printf("--- Block %u %s\n", uid, cold_hint ? "[COLD]" : "");
			for (auto i : *this) {
				printf(LI_GRN "#%-5x" LI_DEF " %s\n", i->source_bc, i->to_string(true).c_str());
			}
		}

		// Domination check.
		//
		bool dom(const basic_block* n) const;
		bool postdom(const basic_block* n) const;

		// Returns true if this block can reach the block.
		//
		bool check_path(const basic_block* to) const;

		// No copy, construction with procedure reference.
		//
		basic_block(procedure* proc) : proc(proc) {}
		basic_block(const basic_block&)            = delete;
		basic_block& operator=(const basic_block&) = delete;

		// Erase all instructions on destruction.
		//
		~basic_block() {
			auto it = begin();
			while (it != end()) {
				it = erase(it);
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
			size_t operator()(const constant& c) const { return std::hash<int64_t>{}(c.i) ^ (size_t) c.vt; }
		};
		std::unordered_map<constant, ref<constant>, const_hash> consts = {};

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
		basic_block* get_entry() const { return basic_blocks.empty() ? nullptr : basic_blocks.front().get(); }

		// Creates a new block.
		//
		basic_block* add_block() {
			is_topologically_sorted = false;
			auto* blk               = basic_blocks.emplace_back(std::make_unique<basic_block>(this)).get();
			blk->uid                = next_block_uid++;
			return blk;
		}
		auto del_block(basic_block* b) {
			auto it = consts.find(constant(b));
			LI_ASSERT(!it->second.use_count());
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
		bool dfs(F&& fn, const basic_block* from = nullptr) const {
			for (auto& b : basic_blocks) {
				b->visited = false;
			}
			auto rec = [&](auto& self, const basic_block* b) -> bool {
				b->visited = true;
				for (auto& s : b->successors)
					if (!s->visited)
						if (self(self, s))
							return true;
				return fn((basic_block*) b);
			};
			return rec(rec, from ? from : get_entry());
		}
		template<typename F>
		bool bfs(F&& fn, const basic_block* from = nullptr) const {
			for (auto& b : basic_blocks) {
				b->visited = false;
			}
			auto rec = [&](auto& self, const basic_block* b) -> bool {
				b->visited = true;
				if (fn((basic_block*) b))
					return true;
				for (auto& s : b->successors)
					if (!s->visited)
						if (self(self, s))
							return true;
				return false;
			};
			return rec(rec, from ? from : get_entry());
		}
		template<typename F>
		bool rdfs(F&& fn, const basic_block* from) const {
			for (auto& b : basic_blocks) {
				b->visited = false;
			}
			auto rec = [&](auto& self, basic_block* b) -> bool {
				b->visited = true;
				for (auto& s : b->predecesors)
					if (!s->visited)
						if (self(self, s))
							return true;
				return fn((basic_block*) b);
			};
			return rec(rec, from);
		}
		template<typename F>
		bool rbfs(F&& fn, const basic_block* from) const {
			for (auto& b : basic_blocks) {
				b->visited = false;
			}
			auto rec = [&](auto& self, basic_block* b) -> bool {
				b->visited = true;
				if (fn((basic_block*) b))
					return true;
				for (auto& s : b->predecesors)
					if (!s->visited)
						if (self(self, s))
							return true;
				return false;
			};
			return rec(rec, from);
		}

		// Topologically sorts the basic block list.
		//
		void toplogical_sort() {
			if (is_topologically_sorted)
				return;
			uint32_t tmp = (uint32_t) basic_blocks.size();
			dfs([&](basic_block* b) {
				b->uid = --tmp;
				return false;
			});
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
				for (auto i : *bb) {
					i->name = next_reg_name++;
				}
			}
		}

		// Validates all basic blocks.
		//
		void validate() {
			LI_ASSERT(get_entry() != nullptr);
			for ( auto& i : basic_blocks ) {
				i->validate();
#if LI_DEBUG
				if (i->predecesors.empty()) {
					LI_ASSERT(i.get() == get_entry());
				}
				for (auto& succ : i->successors) {
					LI_ASSERT(std::find(succ->predecesors.begin(), succ->predecesors.end(), i.get()) != succ->predecesors.end());
				}
				for (auto& pred : i->predecesors) {
					LI_ASSERT(std::find(pred->successors.begin(), pred->successors.end(), i.get()) != pred->successors.end());
				}
#endif
			}
		}

		// Printer.
		//
		void print() {
			printf("---------------------------------\n");
			for (auto& bb : basic_blocks) {
				bb->print();
			}
		}
	};

	// Handles constants.
	//
	template<typename Tv>
	static ref<> launder_value(procedure* proc, Tv&& v) {
		if constexpr (!std::is_convertible_v<Tv, ref<>>) {
			constant c{std::forward<Tv>(v)};
			auto     it = proc->consts.find(c);
			if (it == proc->consts.end()) {
				it = proc->consts.emplace(c, make_value<constant>(c)).first;
			}
			return it->second;
		} else {
			return ref<>(v);
		}
	}

	// Builder context.
	//
	struct builder {
		basic_block* blk        = nullptr;
		bc::pos      current_bc = bc::no_pos;
		builder(basic_block* b = nullptr) : blk(b) {}

		// Instruction emitting.
		//
		template<typename T, typename... Tx>
		ref<T> create(Tx&&... args) {
			// Set basic details.
			//
			ref<T> i     = make_value<T>();
			i->name      = blk->proc->next_reg_name++;
			i->source_bc = current_bc;
			i->opc       = T::Opcode;
			i->vt        = type::unk;

			// Add operands and update.
			//
			i->operands = std::vector<use<>>{launder_value(blk->proc, std::forward<Tx>(args))...};
			return i;
		}
		template<typename T, typename... Tx>
		ref<insn> emit(Tx&&... args) {
			auto i = create<T, Tx...>(std::forward<Tx>(args)...);
			if (!i->has_debug_info())
				i->source_bc = blk->bc_end;
			blk->push_back(i);
			i->update();
			return i;
		}
		template<typename T, typename... Tx>
		ref<insn> emit_front(Tx&&... args) {
			auto i = create<T, Tx...>(std::forward<Tx>(args)...);
			if (!i->has_debug_info())
				i->source_bc = blk->bc_end;
			blk->push_front(i);
			i->update();
			return i;
		}
		template<typename T, typename... Tx>
		ref<insn> emit_after(insn* at, Tx&&... args) {
			auto i = create<T, Tx...>(std::forward<Tx>(args)...);
			at->parent->insert(instruction_iterator(at->next), i);
			if (!i->has_debug_info())
				at->copy_debug_info_to(i);
			i->update();
			return i;
		}
		template<typename T, typename... Tx>
		ref<insn> emit_before(insn* at, Tx&&... args) {
			auto i = create<T, Tx...>(std::forward<Tx>(args)...);
			at->parent->insert(instruction_iterator(at), i);
			if (!i->has_debug_info())
				at->copy_debug_info_to(i);
			i->update();
			return i;
		}
	};
};