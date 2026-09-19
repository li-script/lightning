#include <algorithm>
#include <cstdint>
#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/runtime.hpp>
#include <ir/value.hpp>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vm/inline_cache.hpp>
#include <vm/object.hpp>
#include <vm/string.hpp>

namespace li::ir::opt {
	namespace {
		any_t LI_CC strict_struct_array_access_error(vm* L) { return L->error("invalid strict struct array access"); }

		const nfunc_info strict_struct_array_access_error_info = {
			 func_attr_sideeffect | func_attr_c_takes_vm,
			 "ir.strict_struct_array_access_error",
			 {nfunc_overload{li::bit_cast<const void*>(&strict_struct_array_access_error), {}, type::any}},
		};
	}

	static const nfunc_info inline_cache_field_get_info = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 "ir.inline_cache_field_get",
		 {nfunc_overload{li::bit_cast<const void*>(&inline_cache_field_get), {type::ptr, type::any, type::any}, type::any}},
	};
	static const nfunc_info inline_cache_field_get_raw_info = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 "ir.inline_cache_field_get_raw",
		 {nfunc_overload{li::bit_cast<const void*>(&inline_cache_field_get_raw), {type::ptr, type::any, type::any}, type::any}},
	};
	static const nfunc_info inline_cache_field_set_info = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 "ir.inline_cache_field_set",
		 {nfunc_overload{li::bit_cast<const void*>(&inline_cache_field_set), {type::ptr, type::any, type::any, type::any}, type::any}},
	};
	static const nfunc_info inline_cache_field_set_raw_info = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 "ir.inline_cache_field_set_raw",
		 {nfunc_overload{li::bit_cast<const void*>(&inline_cache_field_set_raw), {type::ptr, type::any, type::any, type::any}, type::any}},
	};

	static bool dominates_instruction(const insn* definition, const insn* use) {
		if (definition->parent != use->parent)
			return definition->parent->dom(use->parent);
		for (auto* instruction : definition->parent->insns()) {
			if (instruction == definition)
				return true;
			if (instruction == use)
				return false;
		}
		return false;
	}

	static std::optional<value_type> get_dominating_type_at(insn* i, value* v) {
		if (v->vt != type::any) {
			return to_value_type(v->vt);
		}

		std::optional<value_type> resolved;
		v->as<insn>()->for_each_user([&](insn* consumer, size_t) {
			if (consumer->is<test_type>()) {
				auto* term = consumer->parent->back();
				if (term->is<jcc>() && term->operands[0] == consumer) {
					if (term->operands[1]->as<constant>()->bb->dom(i->parent)) {
						resolved = consumer->operands[1]->as<constant>()->vty;
						return true;
					}
				}
			}
			if (consumer->is<assume_cast>() && dominates_instruction(consumer, i)) {
				resolved = to_value_type(consumer->operands[1]->as<constant>()->dty);
				return true;
			}
			return false;
		});
		return resolved;
	}

	static void refresh_users(insn* value) {
		std::vector<insn*>        pending;
		std::unordered_set<insn*> visited;
		value->for_each_user([&](insn* user, size_t) {
			pending.emplace_back(user);
			return false;
		});
		while (!pending.empty()) {
			auto* user = pending.back();
			pending.pop_back();
			if (!visited.emplace(user).second)
				continue;
			user->update();
			user->for_each_user([&](insn* next, size_t) {
				pending.emplace_back(next);
				return false;
			});
		}
	}

	static std::pair<ref<insn>, ref<insn>> split_by(insn* i, insn* condition) {
		basic_block* at           = i->parent;
		insn*        split_point  = i->prev;
		auto*        continuation = at->split_at(split_point);
		auto*        true_block   = at->proc->add_block();
		auto*        false_block  = at->proc->add_block();
		builder{split_point}.emit<jcc>(condition, true_block, false_block);
		at->proc->add_jump(at, true_block);
		at->proc->add_jump(at, false_block);

		ref<insn> unchecked(std::in_place, i->duplicate());
		auto      checked     = i->erase();
		auto      true_value  = true_block->push_back(checked);
		auto      false_value = false_block->push_back(unchecked);
		checked->update();
		builder{true_block}.emit<jmp>(continuation);
		builder{false_block}.emit<jmp>(continuation);
		at->proc->add_jump(true_block, continuation);
		at->proc->add_jump(false_block, continuation);

		if (true_value.at->vt != type::none) {
			auto joined = builder{continuation}.emit_front<phi>(true_value.at, false_value.at);
			checked->for_each_user_outside_block([&](insn* user, size_t operand) {
				if (user != joined)
					user->operands[operand].reset(joined);
				return false;
			});
			refresh_users(joined->as<insn>());
		}
		return {std::move(checked), std::move(unchecked)};
	}

	static std::pair<ref<insn>, ref<insn>> split_by(insn* i, size_t operand, value_type expected) {
		builder b{i};
		auto    condition = b.emit_before<test_type>(i, i->operands[operand], expected);
		auto    branches  = split_by(i, condition->as<insn>());
		branches.first->operands[operand] =
			 builder{branches.first}.emit_before<assume_cast>(branches.first, branches.first->operands[operand], to_type(expected));
		branches.first->update();
		refresh_users(branches.first.get());
		return branches;
	}

	static ref<insn> replace_with_runtime(insn* i, inline_cache* cache = nullptr) {
		builder   b{i};
		ref<insn> call;
		if (i->is<unop>()) {
			auto op = int32_t(i->operands[0]->as<constant>()->vmopr);
			call    = b.emit_before<ccall>(i, &runtime::unary_info, 0, i->operands[1], op);
		} else if (i->is<binop>() || i->is<compare>()) {
			auto op = int32_t(i->operands[0]->as<constant>()->vmopr);
			call    = b.emit_before<ccall>(i, &runtime::binary_info, 0, i->operands[1], i->operands[2], op);
		} else {
			const bool get = i->is<field_get>();
			LI_ASSERT(get || i->is<field_set>());
			const bool        raw = i->operands[0]->as<constant>()->i1;
			const nfunc_info* info;
			if (cache) {
				info = get ? (raw ? &inline_cache_field_get_raw_info : &inline_cache_field_get_info)
							  : (raw ? &inline_cache_field_set_raw_info : &inline_cache_field_set_info);
			} else {
				info = get ? (raw ? &runtime::field_get_raw_info : &runtime::field_get_info) : (raw ? &runtime::field_set_raw_info : &runtime::field_set_info);
			}
			call = b.emit_before<ccall>(i, info, 0);
			if (cache)
				call->operands.emplace_back(b.blk->proc->add_const(static_cast<int64_t>(reinterpret_cast<intptr_t>(cache))));
			for (size_t operand = 1; operand != i->operands.size(); ++operand)
				call->operands.emplace_back(i->operands[operand]);
			call->update();
		}
		i->replace_all_uses(call);
		i->erase();
		refresh_users(call.get());
		return call;
	}

	static bool is_struct_value_forwarder(insn* instruction, size_t operand) {
		if (instruction->is<move>() || instruction->is<assume_cast>())
			return operand == 0;
		if (instruction->is<phi>())
			return true;
		return false;
	}

	static bool collect_struct_field_reads(value* source, std::vector<insn*>& forwarding, std::vector<field_get*>& reads, std::unordered_set<insn*>& visited) {
		if (!source->is<insn>())
			return false;
		bool valid = true;
		source->as<insn>()->for_each_user([&](insn* user, size_t operand) {
			if (user->is<field_get>() && operand == 1) {
				reads.push_back(user->as<field_get>());
				return false;
			}
			if (user->is<keep_alive>() && operand == 0) {
				if (visited.emplace(user).second)
					forwarding.push_back(user);
				return false;
			}
			if (!is_struct_value_forwarder(user, operand)) {
				valid = false;
				return true;
			}
			if (visited.emplace(user).second) {
				forwarding.push_back(user);
				if (!collect_struct_field_reads(user, forwarding, reads, visited)) {
					valid = false;
					return true;
				}
			}
			return false;
		});
		return valid;
	}

	static bool redirect_struct_array_failure(jcc* branch) {
		auto* from        = branch->parent;
		auto* proc        = from->proc;
		auto* old_failure = branch->operands[2]->as<constant>()->bb;
		auto  predecessor = range::find(old_failure->predecessors, from);
		if (predecessor == old_failure->predecessors.end())
			return false;
		const size_t            predecessor_index = size_t(predecessor - old_failure->predecessors.begin());
		std::vector<ref<value>> incoming;
		for (auto* phi_value : old_failure->phis())
			incoming.emplace_back(phi_value->operands[predecessor_index]);

		auto* error      = proc->add_block();
		error->cold_hint = 1;
		builder fail{error};
		fail.current_bc = branch->source_bc;
		fail.emit<ccall>(&strict_struct_array_access_error_info, 0);
		fail.emit<jmp>(old_failure);

		proc->del_jump(from, old_failure);
		proc->add_jump(from, error);
		proc->add_jump(error, old_failure);
		branch->operands[2].reset(proc->add_const(error));
		size_t index = 0;
		for (auto* phi_value : old_failure->phis())
			phi_value->operands.emplace_back(incoming[index++]);
		return true;
	}

	static bool fuse_struct_array_get(struct_array_get* element) {
		vclass* expected = element->operands[2]->as<constant>()->vcl;
		if (!expected || !expected->value_semantics || !(element->operands[1]->vt == type::i32 || is_floating_point_data(element->operands[1]->vt)))
			return false;

		std::vector<extract*> data_extracts;
		std::vector<extract*> status_extracts;
		bool                  valid = true;
		element->for_each_user([&](insn* user, size_t operand) {
			if (operand != 0 || !user->is<extract>()) {
				valid = false;
				return true;
			}
			auto* extraction = user->as<extract>();
			if (extraction->operands[1]->as<constant>()->i32 == 0)
				data_extracts.push_back(extraction);
			else
				status_extracts.push_back(extraction);
			return false;
		});
		if (!valid || data_extracts.empty() || status_extracts.empty())
			return false;

		std::vector<insn*>        forwarding;
		std::vector<field_get*>   reads;
		std::unordered_set<insn*> visited;
		for (auto* extraction : data_extracts) {
			forwarding.push_back(extraction);
			visited.emplace(extraction);
			if (!collect_struct_field_reads(extraction, forwarding, reads, visited))
				return false;
		}
		if (reads.empty())
			return false;
		std::unordered_set<field_get*> unique_reads;
		reads.erase(std::remove_if(reads.begin(), reads.end(), [&](field_get* read) { return !unique_reads.emplace(read).second; }), reads.end());

		struct planned_read {
			field_get* instruction;
			msize_t    field_index;
			msize_t    field_offset;
			type       storage;
		};
		std::vector<planned_read> plan;
		plan.reserve(reads.size());
		for (auto* read : reads) {
			if (!read->operands[2]->is<constant>() || read->operands[2]->vt != type::str)
				return false;
			string* key    = read->operands[2]->as<constant>()->str;
			auto    fields = expected->fields();
			auto    field  = std::find_if(fields.begin(), fields.end(), [&](const field_pair& candidate) { return string_value_equals(candidate.key, key); });
			if (field == fields.end() || field->value.is_static || field->value.is_dyn ||
				 !(field->value.ty == type::i1 || is_integer_data(field->value.ty) || is_floating_point_data(field->value.ty)))
				return false;
			plan.push_back({read, msize_t(field - fields.begin()), field->value.offset, field->value.ty});
		}

		std::vector<jcc*> boundary_branches;
		for (auto* status : status_extracts) {
			bool status_valid = true;
			status->for_each_user([&](insn* user, size_t operand) {
				if (!user->is<jcc>() || operand != 0) {
					status_valid = false;
					return true;
				}
				boundary_branches.push_back(user->as<jcc>());
				return false;
			});
			if (!status_valid)
				return false;
		}
		if (boundary_branches.empty())
			return false;

		builder    at{element};
		ref<value> index = element->operands[1];
		if (index->vt != type::i32)
			index = at.emit_before<assume_cast>(element, index, type::i32);
		auto class_valid  = at.emit_before<struct_array_class_test>(element, element->operands[0], expected);
		auto bounds_valid = at.emit_before<struct_array_bounds_test>(element, element->operands[0], index);
		auto access_valid = at.emit_before<bool_and>(element, class_valid, bounds_valid);
		for (auto* status : status_extracts)
			status->replace_all_uses(access_valid);
		for (auto* branch : boundary_branches)
			if (!redirect_struct_array_failure(branch))
				return false;

		const msize_t                          alignment = alignof(any);
		const msize_t                          stride    = (expected->object_length + alignment - 1) & ~(alignment - 1);
		std::unordered_map<msize_t, ref<insn>> loaded_fields;
		builder                                load_builder{data_extracts.front()};
		for (const auto& item : plan) {
			auto [loaded, inserted] = loaded_fields.try_emplace(item.field_index);
			if (inserted) {
				loaded->second = load_builder.emit_before<struct_array_field_load>(
					 data_extracts.front(), element->operands[0], index, int32_t(stride), int32_t(item.field_offset), item.storage);
			}
			item.instruction->replace_all_uses(loaded->second);
			item.instruction->erase();
			refresh_users(loaded->second.get());
		}

		for (auto* status : status_extracts)
			if (status->parent && status->use_count() == 0)
				status->erase();
		for (auto it = forwarding.rbegin(); it != forwarding.rend(); ++it) {
			if ((*it)->parent && (*it)->use_count() == 0)
				(*it)->erase();
		}
		LI_ASSERT(element->use_count() == 0);
		element->erase();
		return true;
	}

	static void fuse_struct_array_elements(procedure* proc) {
		std::vector<ref<struct_array_get>> candidates;
		for (auto& block : proc->basic_blocks)
			for (auto* instruction : block->insns())
				if (instruction->is<struct_array_get>())
					candidates.emplace_back(make_ref(instruction->as<struct_array_get>()));
		for (auto& candidate : candidates)
			if (candidate->parent)
				fuse_struct_array_get(candidate.get());
	}

	static bool numeric(type value) { return is_integer_data(value) || value == type::f64; }

	static bool trait_capable(type value) { return value == type::any || value == type::tbl || value == type::vcl || value <= type::obj; }

	static bool specialize_op(procedure* proc, size_t& split_budget) {
		return proc->bfs([&](basic_block* block) {
			for (auto* candidate : block->insns()) {
				bool needs_specialization = false;
				if (candidate->is<unop>()) {
					auto op              = candidate->operands[0]->as<constant>()->vmopr;
					needs_specialization = op == bc::ANEG && !numeric(candidate->operands[1]->vt);
				} else if (candidate->is<binop>()) {
					needs_specialization = !numeric(candidate->operands[1]->vt) || !numeric(candidate->operands[2]->vt);
				} else if (candidate->is<compare>()) {
					auto op = candidate->operands[0]->as<constant>()->vmopr;
					if (op == bc::CEQ || op == bc::CNE) {
						if (trait_capable(candidate->operands[1]->vt) || trait_capable(candidate->operands[2]->vt)) {
							replace_with_runtime(candidate);
							return true;
						}
					} else {
						needs_specialization = !numeric(candidate->operands[1]->vt) || !numeric(candidate->operands[2]->vt);
					}
				}
				if (!needs_specialization)
					continue;

				// Reuse a dominating successful number guard instead of testing the same
				// boxed value at every arithmetic use. The cast is only placed in a block
				// dominated by the guard's true edge, so it carries no new assumption.
				for (size_t n = 1; n != candidate->operands.size(); ++n) {
					if (candidate->operands[n]->vt != type::any)
						continue;
					auto resolved = get_dominating_type_at(candidate, candidate->operands[n].get());
					if (resolved && *resolved == type_number)
						candidate->operands[n] = builder{candidate}.emit_before<assume_cast>(candidate, candidate->operands[n], type::f64);
				}
				candidate->update();

				size_t unknown_operand = 0;
				for (size_t n = 1; n != candidate->operands.size(); ++n) {
					if (candidate->operands[n]->vt == type::any) {
						unknown_operand = n;
						break;
					}
				}
				if (!unknown_operand || split_budget == 0) {
					replace_with_runtime(candidate);
					return true;
				}

				--split_budget;
				auto [fast, slow] = split_by(candidate, unknown_operand, type_number);
				replace_with_runtime(slow.get());
				fast->update();
				refresh_users(fast.get());
				return true;
			}
			return false;
		});
	}

	static bool specialize_native(procedure* proc) {
		return proc->bfs([&](basic_block* block) {
			for (auto* call : block->insns()) {
				if (!call->is<vcall>() || !call->operands[0]->is<constant>() || call->operands[0]->vt != type::fn)
					continue;
				auto* function = call->operands[0]->as<constant>()->fn;
				auto* info     = function->ninfo;
				if (!info || info->overloads[0].cfunc == nullptr)
					continue;

				size_t argument_offset = (info->attr & func_attr_c_takes_self) ? 1 : 2;
				if (argument_offset > call->operands.size())
					continue;
				auto                  given = std::span{call->operands}.subspan(argument_offset);
				const nfunc_overload* match = nullptr;
				for (const auto& overload : info->get_overloads()) {
					if (overload.args.size() != given.size())
						continue;
					bool compatible = true;
					for (size_t n = 0; n != given.size(); ++n) {
						if (given[n]->vt == type::any || (overload.args[n] != type::any && given[n]->vt != overload.args[n])) {
							compatible = false;
							break;
						}
					}
					if (compatible) {
						match = &overload;
						break;
					}
				}
				if (!match)
					continue;

				builder b{call};
				auto    direct = b.emit_before<ccall>(call, info, int32_t(match - info->overloads.data()));
				for (auto& argument : given)
					direct->operands.emplace_back(argument);
				direct->update();
				call->replace_all_uses(direct);
				call->erase();
				refresh_users(direct.get());
				return true;
			}
			return false;
		});
	}

	void type_split_cfg(procedure* proc) {
		proc->validate();
		fuse_struct_array_elements(proc);
		proc->validate();

		std::vector<insn*> field_sites;
		for (const auto& block : proc->basic_blocks) {
			for (insn* instruction : block->insns()) {
				if (instruction->is<field_get>() || instruction->is<field_set>())
					field_sites.emplace_back(instruction);
			}
		}
		LI_ASSERT(!proc->inline_caches);
		if (proc->cache_fields && !field_sites.empty())
			proc->inline_caches = std::make_unique<inline_cache_array>(field_sites.size(), proc->f && proc->f->shared);

		while (specialize_native(proc))
			proc->validate();

		// Guard splitting duplicates control-flow and, more importantly, multiplies
		// exceptional cleanup state. Beyond this point generic runtime arithmetic is
		// smaller than another fast/slow region and avoids pathological compile time.
		static constexpr size_t max_guarded_splits = 16;
		size_t                  split_budget       = max_guarded_splits;
		while (specialize_op(proc, split_budget))
			proc->validate();

		for (size_t index = 0; index != field_sites.size(); ++index)
			replace_with_runtime(field_sites[index], proc->inline_caches ? &(*proc->inline_caches)[index] : nullptr);
		proc->validate();
	}

	void type_inference(procedure* proc) {
		bool changed = false;
		for (auto& block : proc->basic_blocks) {
			for (auto* i : block->insns()) {
				if (!i->is<test_type>())
					continue;
				auto expected = i->operands[1]->as<constant>()->vty;
				auto resolved = get_dominating_type_at(i, i->operands[0]);
				if (resolved) {
					i->replace_all_uses(launder_value(proc, *resolved == expected));
					changed = true;
				}
			}
		}
		if (changed) {
			dce(proc);
			cfg(proc);
		}
	}
};
