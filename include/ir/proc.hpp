#pragma once
#include <ir/insn.hpp>
#include <list>
#include <ranges>
#include <string>
#include <unordered_map>
#include <util/llist.hpp>
#include <vector>
#include <vm/bc.hpp>
#include <vm/function.hpp>
#include <vm/inline_cache.hpp>

namespace li::ir {
	struct instruction_iterator {
		using iterator_category = std::bidirectional_iterator_tag;
		using difference_type   = ptrdiff_t;
		using value_type        = insn*;
		using pointer           = insn*;
		using reference         = insn*;

		insn* at;
		instruction_iterator(insn* at = nullptr) : at(at) {}

		reference             operator*() const { return at; }
		pointer               operator->() const { return at; }
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
		friend bool operator==(const instruction_iterator& a, const instruction_iterator& b) { return a.at == b.at; }
	};

	// Basic block type.
	//
	struct basic_block : range::view_base {
		// Range traits.
		//
		using iterator = instruction_iterator;

		// Unique identifier, note that this may change on sort.
		//
		msize_t uid = 0;

		// Details of the basic block itself.
		//
		procedure* proc       = nullptr;  // Procedure it belongs to.
		uint8_t    cold_hint  = 0;        // Number specifying how cold this block is.
		uint8_t    loop_depth = 0;        // Number of nested loops we're in.

		// Bytecode ranges.
		//
		bc::pos bc_begin = 0;
		bc::pos bc_end   = 0;

		// Successor and predecesor list.
		//
		std::vector<basic_block*> successors   = {};
		std::vector<basic_block*> predecessors = {};

		// Instruction list.
		//
		mutable insn insn_list_head = {};

		// Temporary for search algorithms.
		//
		mutable uint64_t visited = 0;

		// Cached dominator-tree intervals. Zero denotes a block outside the
		// corresponding analysis (for example, an unreachable block).
		//
		mutable msize_t dominator_pre      = 0;
		mutable msize_t dominator_post     = 0;
		mutable msize_t postdominator_pre  = 0;
		mutable msize_t postdominator_post = 0;

		// Container observers.
		//
		instruction_iterator begin() const { return {insn_list_head.next}; }
		instruction_iterator end() const { return {&insn_list_head}; }
		auto                 rbegin() const { return std::reverse_iterator(end()); }
		auto                 rend() const { return std::reverse_iterator(begin()); }
		bool                 empty() const { return insn_list_head.next == &insn_list_head; }
		insn*                front() const { return (!empty()) ? make_ref(insn_list_head.next) : nullptr; }
		insn*                back() const { return (!empty()) ? make_ref(insn_list_head.prev) : nullptr; }
		instruction_iterator end_phi() const {
			return std::find_if(begin(), end(), [](insn* i) { return !i->is<phi>(); });
		}
		auto phis() const { return range::subrange(begin(), end_phi()); }
		auto insns() const { return range::subrange(begin(), end()); }
		auto after(insn* i) const { return range::subrange(i ? instruction_iterator(i->next) : begin(), end()); }
		auto before(insn* i) const { return range::subrange(begin(), i ? instruction_iterator(i) : end()); }

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

		// Splits the basic block at instruction boundary returns the new block. User must add the new terminator.
		//
		basic_block* split_at(const insn* at);

		// Validates the containing procedure so cross-block ownership, CFG, and
		// SSA invariants are checked even at block-scoped pipeline points.
		//
		void validate();

		// Printer.
		//
		void print() const {
			printf("-- Block $%x", uid);
			if (cold_hint)
				printf(LI_CYN " [COLD %u]" LI_DEF, (uint32_t) cold_hint);
			if (loop_depth)
				printf(LI_RED " [LOOP %u]" LI_DEF, (uint32_t) loop_depth);
			putchar('\n');

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
	static std::string to_string(basic_block* bb) { return util::fmt(LI_PRP "$%x" LI_DEF, bb->uid); }

	// Procedure type.
	//
	struct procedure : range::view_base {
		// Range traits.
		//
		using container = std::vector<std::unique_ptr<basic_block>>;
		using iterator  = typename container::iterator;

		// Source information.
		//
		vm*             L = nullptr;  // Related VM and function.
		function_proto* f = nullptr;  //

		// Constant pool.
		//
		struct const_hash {
			size_t operator()(const constant& c) const { return std::hash<int64_t>{}(c.i) ^ (size_t) c.vt; }
		};
		std::unordered_map<constant, ref<constant>, const_hash> consts = {};

		// Procedure state.
		//
		container                           basic_blocks;                 // List of basic blocks, first is entry point.
		std::unique_ptr<inline_cache_array> inline_caches;                // Stable generated-code field feedback.
		bool                                cache_fields   = true;        // Enables field feedback for this compilation.
		uint32_t                            osr_target     = bc::no_pos;  // Loop header reachable through the OSR entry, if any.
		msize_t                             next_reg_name  = 0;           // Next register name.
		msize_t                             next_block_uid = 0;           // Next block uid.

		// Maximum local index for VCALL.
		//
		msize_t max_stack_slot = 0;

		// Analysis data.
		//
		uint32_t         is_topologically_sorted : 1 = 0;
		mutable uint64_t next_visited_mark           = 0x50eaeb7446b52b12;
		uint64_t         cfg_revision                = 1;
		mutable uint64_t dominator_revision          = 0;
		mutable uint64_t postdominator_revision      = 0;

		// Constructed by VM instance and the function we're translating.
		//
		procedure(vm* L, function_proto* f) : L(L), f(f) {}

		// Duplicates the procedure.
		//
		std::unique_ptr<procedure> duplicate();

		// Container observers.
		//
		iterator begin() { return basic_blocks.begin(); }
		iterator end() { return basic_blocks.end(); }
		size_t   size() { return basic_blocks.size(); }
		bool     empty() { return basic_blocks.empty(); }

		// Adds a new constant.
		//
		ref<constant> add_const(constant c) {
			auto it = consts.find(c);
			if (it == consts.end()) {
				it = consts.emplace(c, make_value<constant>(c)).first;
			}
			return it->second;
		}

		// Clears visitor state.
		//
		void clear_block_visitor_state() const {
			for (auto& b : basic_blocks) {
				b->visited = 0;
			}
		}
		void clear_all_visitor_state() const {
			for (auto& b : basic_blocks) {
				for (auto i : b->insns())
					i->visited = 0;
				b->visited = 0;
			}
		}

		// Gets the entry point.
		//
		basic_block* get_entry() const { return basic_blocks.empty() ? nullptr : basic_blocks.front().get(); }

		// Creates a new block.
		//
		basic_block* add_block() {
			auto* blk = basic_blocks.emplace_back(std::make_unique<basic_block>(this)).get();
			blk->uid  = next_block_uid++;
			mark_blocks_dirty();
			return blk;
		}
		auto del_block(basic_block* b) {
			auto it = consts.find(constant(b));
			if (it != consts.end()) {
				LI_ASSERT(!it->second.use_count());
				consts.erase(it);
			}
			LI_ASSERT(b->predecessors.empty());
			LI_ASSERT(b->successors.empty());
			mark_blocks_dirty();
			for (auto it = basic_blocks.begin();; ++it) {
				LI_ASSERT(it != basic_blocks.end());
				if (it->get() == b) {
					return basic_blocks.erase(it);
				}
			}
		}

		// Dirties all block analysis.
		//
		void mark_blocks_dirty() {
			is_topologically_sorted = false;
			if (++cfg_revision == 0)
				cfg_revision = 1;
			dominator_revision     = 0;
			postdominator_revision = 0;
		}

		// Lazily rebuilds cached dominance intervals for the current CFG.
		//
		void ensure_dominators() const;
		void ensure_postdominators() const;

		// Adds or deletes a jump.
		//
		void add_jump(basic_block* from, basic_block* to) {
			auto sit = range::find(from->successors, to);
			auto pit = range::find(to->predecessors, from);
			from->successors.emplace_back(to);
			to->predecessors.emplace_back(from);
			mark_blocks_dirty();
		}
		void del_jump(basic_block* from, basic_block* to, bool fix_phi = true) {
			auto sit = range::find(from->successors, to);
			auto pit = range::find(to->predecessors, from);
			LI_ASSERT(sit != from->successors.end());
			LI_ASSERT(pit != to->predecessors.end());
			if (fix_phi) {
				size_t n = pit - to->predecessors.begin();
				for (auto phi : to->phis()) {
					LI_ASSERT(phi->operands.size() == to->predecessors.size());
					phi->operands.erase(phi->operands.begin() + n);
				}
			}
			from->successors.erase(sit);
			to->predecessors.erase(pit);
			mark_blocks_dirty();
		}

		// Removes every block that cannot be reached from the entry point.
		//
		size_t remove_unreachable_blocks() {
			if (basic_blocks.empty())
				return 0;

			const auto                mark = ++next_visited_mark;
			std::vector<basic_block*> worklist{get_entry()};
			get_entry()->visited = mark;
			for (size_t i = 0; i != worklist.size(); ++i) {
				for (auto* successor : worklist[i]->successors) {
					if (successor->visited != mark) {
						successor->visited = mark;
						worklist.emplace_back(successor);
					}
				}
			}

			std::vector<basic_block*> unreachable;
			for (auto& block : basic_blocks) {
				if (block->visited != mark)
					unreachable.emplace_back(block.get());
			}

			// First detach every dead edge. This makes the predecessor lists
			// empty even for unreachable strongly connected components.
			//
			for (auto* block : unreachable) {
				while (!block->successors.empty())
					del_jump(block, block->successors.back());
			}

			// Break operand cycles first, then drop all instructions. This also
			// releases constants naming blocks in a dead cycle before any owner
			// is deleted.
			//
			for (auto* block : unreachable) {
				LI_ASSERT(block->predecessors.empty());
				for (auto* instruction : *block)
					instruction->operands.clear();
			}
			for (auto* block : unreachable) {
				while (!block->empty())
					block->erase(block->begin());
			}
			for (auto* block : unreachable)
				del_block(block);
			return unreachable.size();
		}

		// Templated DFS/BFS helper.
		//
		template<typename F>
		bool dfs(F&& fn, const basic_block* from = nullptr) const {
			auto mark = ++next_visited_mark;
			auto rec  = [&](auto& self, const basic_block* b) -> bool {
				b->visited = mark;
				for (auto& s : b->successors)
					if (s->visited != mark)
						if (self(self, s))
							return true;
				return fn((basic_block*) b);
			};

			if (from) {
				for (auto& s : from->successors)
					if (rec(rec, s))
						return true;
				return false;
			} else {
				return rec(rec, get_entry());
			}
		}
		template<typename F>
		bool bfs(F&& fn, const basic_block* from = nullptr) const {
			auto mark = ++next_visited_mark;
			auto rec  = [&](auto& self, const basic_block* b) -> bool {
				b->visited = mark;
				if (fn((basic_block*) b))
					return true;
				for (auto& s : b->successors)
					if (s->visited != mark)
						if (self(self, s))
							return true;
				return false;
			};

			if (from) {
				for (auto& s : from->successors)
					if (rec(rec, s))
						return true;
				return false;
			} else {
				return rec(rec, get_entry());
			}
		}

		// Topologically sorts the basic block list.
		//
		void topological_sort() {
			if (is_topologically_sorted)
				return;
			msize_t tmp = (msize_t) basic_blocks.size();
			dfs([&](basic_block* b) {
				b->uid = --tmp;
				return false;
			});
			LI_ASSERT(get_entry()->uid == 0);
			std::sort(basic_blocks.begin(), basic_blocks.end(), [](auto& a, auto& b) { return a->uid < b->uid; });
			is_topologically_sorted = true;
		}

		// Renames all registers and blocks.
		//
		void reset_names() {
			next_reg_name = 0;
			for (auto& bb : basic_blocks) {
				for (auto i : *bb) {
					i->name = next_reg_name++;
				}
			}
		}

		// Validates complete CFG and SSA invariants. Pre-SSA load/store instructions
		// remain valid memory operations and do not weaken instruction-value checks.
		//
		void validate();

		// Printer.
		//
		void print() {
			for (auto& bb : basic_blocks) {
				bb->print();
			}
		}
	};

	// Handles constants.
	//
	template<typename Tv>
	static ref<> launder_value(procedure* proc, Tv&& v) {
		if constexpr (std::is_convertible_v<Tv, const value*>) {
			return make_ref(const_cast<value*>(static_cast<const value*>(v)));
		} else if constexpr (!std::is_convertible_v<Tv, ref<>>) {
			return proc->add_const(constant{std::forward<Tv>(v)});
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
		builder(insn* i) : blk(i->parent), current_bc(i->source_bc) {}

		// Instruction emitting.
		//
		template<typename T, typename... Tx>
		ref<T> create(procedure* p, Tx&&... args) {
			// Set basic details.
			//
			ref<T> i     = make_value<T>();
			i->name      = p->next_reg_name++;
			i->source_bc = current_bc;
			i->opc       = T::Opcode;
			i->vt        = type::any;

			// Add operands and update.
			//
			i->operands = std::vector<use<>>{launder_value(p, std::forward<Tx>(args))...};
			return i;
		}
		template<typename T, typename... Tx>
		ref<insn> emit(Tx&&... args) {
			auto i = create<T, Tx...>(blk->proc, std::forward<Tx>(args)...);
			if (!i->has_debug_info())
				i->source_bc = blk->bc_end;
			blk->push_back(i);
			i->update();
			return i;
		}
		template<typename T, typename... Tx>
		ref<insn> emit_front(Tx&&... args) {
			auto i = create<T, Tx...>(blk->proc, std::forward<Tx>(args)...);
			if (!i->has_debug_info())
				i->source_bc = blk->bc_end;
			blk->push_front(i);
			i->update();
			return i;
		}
		template<typename T, typename... Tx>
		ref<insn> emit_after(insn* at, Tx&&... args) {
			auto i = create<T, Tx...>(at->parent->proc, std::forward<Tx>(args)...);
			at->parent->insert(instruction_iterator(at->next), i);
			if (!i->has_debug_info())
				at->copy_debug_info_to(i);
			i->update();
			return i;
		}
		template<typename T, typename... Tx>
		ref<insn> emit_before(insn* at, Tx&&... args) {
			auto i = create<T, Tx...>(at->parent->proc, std::forward<Tx>(args)...);
			at->parent->insert(instruction_iterator(at), i);
			if (!i->has_debug_info())
				at->copy_debug_info_to(i);
			i->update();
			return i;
		}
	};
};