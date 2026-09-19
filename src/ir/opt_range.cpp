#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ir/insn.hpp>
#include <ir/opt.hpp>
#include <ir/proc.hpp>
#include <ir/value.hpp>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace li::ir::opt {
	namespace {
		constexpr int64_t exact_integer_limit       = int64_t{1} << 53;
		constexpr size_t  maximum_loops             = 64;
		constexpr size_t  maximum_loop_blocks       = 64;
		constexpr size_t  maximum_loop_instructions = 512;
		constexpr size_t  maximum_candidates        = 256;
		constexpr size_t  maximum_analysis_passes   = 64;
		constexpr size_t  maximum_transparent_depth = 8;
		constexpr size_t  maximum_boundary_casts    = 32;
		constexpr size_t  maximum_loop_search_edges = 4096;

		struct integer_interval {
			int64_t minimum = 0;
			int64_t maximum = 0;
		};

		using interval_map = std::unordered_map<insn*, integer_interval>;

		struct natural_loop {
			basic_block*                     header = nullptr;
			std::unordered_set<basic_block*> blocks;
			bool                             over_budget = false;
		};

		struct induction_proof {
			insn*               induction = nullptr;
			integer_interval    values;
			int64_t             maximum_iterations = 0;
			std::vector<insn*>  recurrences;
			std::vector<value*> incoming_values;
		};

		enum class boundary_kind : uint8_t {
			to_f64,
			erase_only,
		};

		struct boundary_use {
			insn*         user    = nullptr;
			size_t        operand = 0;
			boundary_kind kind    = boundary_kind::to_f64;
		};

		static std::optional<integer_interval> exact_constant(value* candidate) {
			if (!candidate->is<constant>())
				return std::nullopt;
			auto* constant = candidate->as<ir::constant>();
			if (is_integer_data(constant->vt))
				return integer_interval{constant->i, constant->i};
			if (constant->vt != type::f64)
				return std::nullopt;

			double number = constant->n;
			if (!std::isfinite(number) || std::trunc(number) != number)
				return std::nullopt;
			// Integer zero is boxed as +0. Reject -0 even though it compares equal.
			if (number == 0 && std::signbit(number))
				return std::nullopt;
			if (number < -double(exact_integer_limit) || number > double(exact_integer_limit))
				return std::nullopt;
			auto integer = int64_t(number);
			return integer_interval{integer, integer};
		}

		static std::optional<int64_t> checked_add(int64_t left, int64_t right) {
			if ((right > 0 && left > std::numeric_limits<int64_t>::max() - right) || (right < 0 && left < std::numeric_limits<int64_t>::min() - right))
				return std::nullopt;
			return left + right;
		}

		static std::optional<int64_t> checked_multiply(int64_t left, int64_t right) {
			bool     negative        = (left < 0) != (right < 0);
			uint64_t left_magnitude  = left < 0 ? uint64_t(-(left + 1)) + 1 : uint64_t(left);
			uint64_t right_magnitude = right < 0 ? uint64_t(-(right + 1)) + 1 : uint64_t(right);
			uint64_t limit           = negative ? uint64_t(std::numeric_limits<int64_t>::max()) + 1 : uint64_t(std::numeric_limits<int64_t>::max());
			if (left_magnitude && right_magnitude > limit / left_magnitude)
				return std::nullopt;
			uint64_t product = left_magnitude * right_magnitude;
			if (!negative)
				return int64_t(product);
			if (product == uint64_t(std::numeric_limits<int64_t>::max()) + 1)
				return std::numeric_limits<int64_t>::min();
			return -int64_t(product);
		}

		static std::optional<integer_interval> checked_interval(int64_t minimum, int64_t maximum) {
			if (minimum > maximum || minimum < -exact_integer_limit || maximum > exact_integer_limit)
				return std::nullopt;
			return integer_interval{minimum, maximum};
		}

		static integer_interval unite(integer_interval left, integer_interval right) {
			return {std::min(left.minimum, right.minimum), std::max(left.maximum, right.maximum)};
		}

		static bool contains_zero(integer_interval value) { return value.minimum <= 0 && value.maximum >= 0; }

		static std::optional<integer_interval> apply(operation op, integer_interval left, integer_interval right) {
			switch (op) {
				case bc::AADD: {
					auto minimum = checked_add(left.minimum, right.minimum);
					auto maximum = checked_add(left.maximum, right.maximum);
					return minimum && maximum ? checked_interval(*minimum, *maximum) : std::nullopt;
				}
				case bc::ASUB: {
					auto minimum = checked_add(left.minimum, -right.maximum);
					auto maximum = checked_add(left.maximum, -right.minimum);
					return minimum && maximum ? checked_interval(*minimum, *maximum) : std::nullopt;
				}
				case bc::AMUL: {
					// With only +0 inputs, IEEE multiplication creates -0 exactly when a
					// zero is multiplied by a negative value. Integer multiplication cannot
					// preserve that sign, so do not specialize such a product.
					if ((contains_zero(left) && right.minimum < 0) || (contains_zero(right) && left.minimum < 0))
						return std::nullopt;
					const std::optional<int64_t> products[] = {
						 checked_multiply(left.minimum, right.minimum),
						 checked_multiply(left.minimum, right.maximum),
						 checked_multiply(left.maximum, right.minimum),
						 checked_multiply(left.maximum, right.maximum),
					};
					int64_t minimum = exact_integer_limit;
					int64_t maximum = -exact_integer_limit;
					for (auto product : products) {
						if (!product || *product < -exact_integer_limit || *product > exact_integer_limit)
							return std::nullopt;
						minimum = std::min(minimum, *product);
						maximum = std::max(maximum, *product);
					}
					return checked_interval(minimum, maximum);
				}
				default:
					return std::nullopt;
			}
		}

		static value* transparent_source(value* candidate) {
			if (!candidate->is<insn>())
				return nullptr;
			auto* instruction = candidate->as<insn>();
			if (instruction->is<move>())
				return instruction->operands[0].get();
			if (instruction->is<extract>() && instruction->operands[1]->as<constant>()->i32 == 0)
				return instruction->operands[0].get();
			return nullptr;
		}

		static value* peel_transparent(value* candidate) {
			for (size_t depth = 0; depth != maximum_transparent_depth; ++depth) {
				auto* source = transparent_source(candidate);
				if (!source)
					return candidate;
				candidate = source;
			}
			return nullptr;
		}

		static std::optional<integer_interval> interval_of(value* candidate, const interval_map& intervals) {
			for (size_t depth = 0; depth != maximum_transparent_depth; ++depth) {
				if (candidate->is<constant>())
					return exact_constant(candidate);
				if (!candidate->is<insn>())
					return std::nullopt;
				auto found = intervals.find(candidate->as<insn>());
				if (found != intervals.end())
					return found->second;
				auto* source = transparent_source(candidate);
				if (!source)
					return std::nullopt;
				candidate = source;
			}
			return std::nullopt;
		}

		static bool add_interval(interval_map& intervals, insn* instruction, integer_interval value) {
			if (intervals.contains(instruction) || intervals.size() >= maximum_candidates)
				return false;
			intervals.emplace(instruction, value);
			return true;
		}

		static bool mark_transparent_chain(interval_map& intervals, value* incoming, insn* core, integer_interval value) {
			insn*  pending[maximum_transparent_depth];
			size_t pending_count = 0;
			for (size_t depth = 0; depth != maximum_transparent_depth; ++depth) {
				if (!incoming->is<insn>())
					return false;
				auto* instruction = incoming->as<insn>();
				if (instruction == core) {
					if (intervals.size() + pending_count > maximum_candidates)
						return false;
					for (size_t index = 0; index != pending_count; ++index)
						intervals.emplace(pending[index], value);
					return true;
				}
				if (instruction->vt != type::f64 || !transparent_source(instruction))
					return false;
				if (!intervals.contains(instruction))
					pending[pending_count++] = instruction;
				incoming = transparent_source(instruction);
			}
			return false;
		}

		static std::vector<natural_loop> find_natural_loops(procedure* proc) {
			std::vector<natural_loop>                loops;
			std::unordered_map<basic_block*, size_t> loop_indices;
			size_t                                   searched_edges = 0;

			for (auto& tail_owner : proc->basic_blocks) {
				auto* tail = tail_owner.get();
				for (auto* header : tail->successors) {
					if (++searched_edges > maximum_loop_search_edges)
						return loops;
					if (!header->dom(tail))
						continue;

					auto [position, inserted] = loop_indices.emplace(header, loops.size());
					if (inserted) {
						if (loops.size() == maximum_loops)
							return loops;
						loops.push_back(natural_loop{header, {header}});
					}
					auto& loop = loops[position->second];
					if (loop.over_budget)
						continue;

					std::vector<basic_block*> worklist;
					if (loop.blocks.emplace(tail).second && tail != header)
						worklist.emplace_back(tail);
					while (!worklist.empty()) {
						if (loop.blocks.size() > maximum_loop_blocks) {
							loop.over_budget = true;
							break;
						}
						auto* block = worklist.back();
						worklist.pop_back();
						for (auto* predecessor : block->predecessors) {
							if (++searched_edges > maximum_loop_search_edges) {
								loop.over_budget = true;
								break;
							}
							if (loop.blocks.emplace(predecessor).second && predecessor != header)
								worklist.emplace_back(predecessor);
						}
						if (loop.over_budget)
							break;
					}
				}
			}
			return loops;
		}

		static std::optional<operation> swap_relation(operation relation) {
			switch (relation) {
				case bc::CEQ:
				case bc::CNE:
					return relation;
				case bc::CLT:
					return bc::CGT;
				case bc::CLE:
					return bc::CGE;
				case bc::CGT:
					return bc::CLT;
				case bc::CGE:
					return bc::CLE;
				default:
					return std::nullopt;
			}
		}

		static std::optional<operation> negate_relation(operation relation) {
			switch (relation) {
				case bc::CEQ:
					return bc::CNE;
				case bc::CNE:
					return bc::CEQ;
				case bc::CLT:
					return bc::CGE;
				case bc::CLE:
					return bc::CGT;
				case bc::CGT:
					return bc::CLE;
				case bc::CGE:
					return bc::CLT;
				default:
					return std::nullopt;
			}
		}

		static bool dominates_backedges(const natural_loop& loop, basic_block* block) {
			for (auto* predecessor : loop.header->predecessors) {
				if (loop.blocks.contains(predecessor) && !block->dom(predecessor))
					return false;
			}
			return true;
		}

		static std::optional<induction_proof> prove_induction(const natural_loop& loop, insn* candidate) {
			if (!candidate->is<phi>() || candidate->parent != loop.header || candidate->vt != type::f64 ||
				 candidate->operands.size() != loop.header->predecessors.size())
				return std::nullopt;

			std::optional<int64_t> initial;
			std::optional<int64_t> step;
			std::vector<insn*>     recurrences;
			std::vector<value*>    incoming_values;
			for (size_t index = 0; index != loop.header->predecessors.size(); ++index) {
				auto* incoming = candidate->operands[index].get();
				if (!loop.blocks.contains(loop.header->predecessors[index])) {
					auto value = exact_constant(incoming);
					if (!value || value->minimum != value->maximum || (initial && *initial != value->minimum))
						return std::nullopt;
					initial = value->minimum;
					continue;
				}

				auto* peeled = peel_transparent(incoming);
				if (!peeled || !peeled->is<insn>())
					return std::nullopt;
				auto* recurrence = peeled->as<insn>();
				if (!recurrence->is<binop>() || recurrence->vt != type::f64 || !loop.blocks.contains(recurrence->parent))
					return std::nullopt;
				auto                            operation = recurrence->operands[0]->as<constant>()->vmopr;
				auto*                           left      = recurrence->operands[1].get();
				auto*                           right     = recurrence->operands[2].get();
				std::optional<integer_interval> amount;
				int64_t                         recurrence_step = 0;
				if (operation == bc::AADD && left == candidate) {
					amount          = exact_constant(right);
					recurrence_step = amount ? amount->minimum : 0;
				} else if (operation == bc::AADD && right == candidate) {
					amount          = exact_constant(left);
					recurrence_step = amount ? amount->minimum : 0;
				} else if (operation == bc::ASUB && left == candidate) {
					amount          = exact_constant(right);
					recurrence_step = amount ? -amount->minimum : 0;
				} else {
					return std::nullopt;
				}
				if (!amount || amount->minimum != amount->maximum || recurrence_step == 0 || (step && *step != recurrence_step))
					return std::nullopt;
				step = recurrence_step;
				recurrences.emplace_back(recurrence);
				incoming_values.emplace_back(incoming);
			}
			if (!initial || !step || recurrences.empty())
				return std::nullopt;

			std::optional<operation> relation;
			std::optional<int64_t>   bound;
			for (auto* block : loop.blocks) {
				auto* terminator = block->back();
				if (!terminator || !terminator->is<jcc>() || !dominates_backedges(loop, block))
					continue;
				auto* true_target  = terminator->operands[1]->as<constant>()->bb;
				auto* false_target = terminator->operands[2]->as<constant>()->bb;
				bool  true_inside  = loop.blocks.contains(true_target);
				bool  false_inside = loop.blocks.contains(false_target);
				if (true_inside == false_inside)
					continue;

				auto* condition = peel_transparent(terminator->operands[0].get());
				if (!condition || !condition->is<insn>() || !condition->as<insn>()->is<compare>())
					continue;
				auto*                           comparison = condition->as<insn>();
				auto                            compared   = comparison->operands[0]->as<constant>()->vmopr;
				auto*                           left       = peel_transparent(comparison->operands[1].get());
				auto*                           right      = peel_transparent(comparison->operands[2].get());
				std::optional<integer_interval> limit;
				if (left == candidate) {
					limit = exact_constant(right);
				} else if (right == candidate) {
					limit        = exact_constant(left);
					auto swapped = swap_relation(compared);
					if (!swapped)
						continue;
					compared = *swapped;
				} else {
					continue;
				}
				if (!limit || limit->minimum != limit->maximum)
					continue;
				if (!true_inside) {
					auto negated = negate_relation(compared);
					if (!negated)
						continue;
					compared = *negated;
				}
				if (relation)
					return std::nullopt;
				relation = compared;
				bound    = limit->minimum;
			}
			if (!relation || !bound)
				return std::nullopt;

			const int64_t start  = *initial;
			const int64_t limit  = *bound;
			const int64_t stride = *step;
			int64_t       iterations;
			if (stride > 0 && *relation == bc::CLT && start < limit) {
				iterations = (limit - start + stride - 1) / stride;
			} else if (stride > 0 && *relation == bc::CLE && start <= limit) {
				iterations = (limit - start) / stride + 1;
			} else if (stride < 0 && *relation == bc::CGT && start > limit) {
				auto magnitude = -stride;
				iterations     = (start - limit + magnitude - 1) / magnitude;
			} else if (stride < 0 && *relation == bc::CGE && start >= limit) {
				auto magnitude = -stride;
				iterations     = (start - limit) / magnitude + 1;
			} else {
				// A zero-trip loop has no executed recurrence to optimize. Relations
				// inconsistent with the step do not establish finite execution.
				return std::nullopt;
			}

			auto total_change = checked_multiply(iterations, stride);
			auto final_value  = total_change ? checked_add(start, *total_change) : std::nullopt;
			if (!final_value)
				return std::nullopt;
			auto values = checked_interval(std::min(start, *final_value), std::max(start, *final_value));
			if (!values)
				return std::nullopt;
			return induction_proof{candidate, *values, iterations, std::move(recurrences), std::move(incoming_values)};
		}

		static bool prove_accumulator(const natural_loop& loop, insn* candidate, int64_t maximum_iterations, interval_map& intervals) {
			if (!candidate->is<phi>() || candidate->parent != loop.header || candidate->vt != type::f64 ||
				 candidate->operands.size() != loop.header->predecessors.size())
				return false;

			std::optional<integer_interval> initial;
			std::optional<integer_interval> delta;
			std::vector<insn*>              recurrences;
			std::vector<value*>             incoming_values;
			for (size_t index = 0; index != loop.header->predecessors.size(); ++index) {
				auto* incoming = candidate->operands[index].get();
				if (!loop.blocks.contains(loop.header->predecessors[index])) {
					auto value = exact_constant(incoming);
					if (!value)
						return false;
					initial = initial ? unite(*initial, *value) : *value;
					continue;
				}

				auto* peeled = peel_transparent(incoming);
				if (!peeled || !peeled->is<insn>())
					return false;
				auto* recurrence = peeled->as<insn>();
				if (!recurrence->is<binop>() || recurrence->vt != type::f64 || !loop.blocks.contains(recurrence->parent))
					return false;
				auto                            operation = recurrence->operands[0]->as<constant>()->vmopr;
				auto*                           left      = recurrence->operands[1].get();
				auto*                           right     = recurrence->operands[2].get();
				std::optional<integer_interval> change;
				if (operation == bc::AADD && left == candidate) {
					change = interval_of(right, intervals);
				} else if (operation == bc::AADD && right == candidate) {
					change = interval_of(left, intervals);
				} else if (operation == bc::ASUB && left == candidate) {
					auto subtrahend = interval_of(right, intervals);
					if (subtrahend)
						change = integer_interval{-subtrahend->maximum, -subtrahend->minimum};
				} else {
					return false;
				}
				if (!change)
					return false;
				delta = delta ? unite(*delta, *change) : *change;
				recurrences.emplace_back(recurrence);
				incoming_values.emplace_back(incoming);
			}
			if (!initial || !delta || recurrences.empty())
				return false;

			std::optional<int64_t> lower_change = int64_t{0};
			std::optional<int64_t> upper_change = int64_t{0};
			if (delta->minimum < 0)
				lower_change = checked_multiply(maximum_iterations, delta->minimum);
			if (delta->maximum > 0)
				upper_change = checked_multiply(maximum_iterations, delta->maximum);
			auto minimum = lower_change ? checked_add(initial->minimum, *lower_change) : std::nullopt;
			auto maximum = upper_change ? checked_add(initial->maximum, *upper_change) : std::nullopt;
			auto range   = minimum && maximum ? checked_interval(*minimum, *maximum) : std::nullopt;
			if (!range || intervals.size() + recurrences.size() + incoming_values.size() * maximum_transparent_depth + 1 > maximum_candidates)
				return false;

			intervals.emplace(candidate, *range);
			for (auto* recurrence : recurrences)
				intervals.emplace(recurrence, *range);
			for (size_t index = 0; index != incoming_values.size(); ++index) {
				if (!mark_transparent_chain(intervals, incoming_values[index], recurrences[index], *range))
					return false;
			}
			return true;
		}

		static bool collect_intervals(
			 const natural_loop& loop, const std::vector<insn*>& instructions, const induction_proof& induction, interval_map& intervals) {
			if (!add_interval(intervals, induction.induction, induction.values))
				return false;
			for (auto* recurrence : induction.recurrences) {
				if (!intervals.contains(recurrence) && !add_interval(intervals, recurrence, induction.values))
					return false;
			}
			for (size_t index = 0; index != induction.incoming_values.size(); ++index) {
				if (!mark_transparent_chain(intervals, induction.incoming_values[index], induction.recurrences[index], induction.values))
					return false;
			}

			for (size_t pass = 0; pass != maximum_analysis_passes; ++pass) {
				bool changed = false;
				for (auto* instruction : instructions) {
					if (intervals.contains(instruction) || instruction->vt != type::f64)
						continue;
					if (auto* source = transparent_source(instruction)) {
						auto source_interval = interval_of(source, intervals);
						if (source_interval) {
							if (!add_interval(intervals, instruction, *source_interval))
								return false;
							changed = true;
						}
						continue;
					}
					if (!instruction->is<binop>())
						continue;
					auto operation = instruction->operands[0]->as<constant>()->vmopr;
					if (operation != bc::AADD && operation != bc::ASUB && operation != bc::AMUL)
						continue;
					auto left  = interval_of(instruction->operands[1], intervals);
					auto right = interval_of(instruction->operands[2], intervals);
					if (!left || !right)
						continue;
					auto result = apply(operation, *left, *right);
					if (result) {
						if (!add_interval(intervals, instruction, *result))
							return false;
						changed = true;
					}
				}

				for (auto* phi : loop.header->phis()) {
					if (!intervals.contains(phi) && prove_accumulator(loop, phi, induction.maximum_iterations, intervals))
						changed = true;
				}
				if (!changed)
					return true;
			}
			// A dependency chain deeper than the fixed budget is left as f64.
			return false;
		}

		static bool fits(type target, integer_interval value) {
			if (target == type::i64)
				return true;
			return value.minimum >= std::numeric_limits<int32_t>::min() && value.maximum <= std::numeric_limits<int32_t>::max();
		}

		static bool is_selected(value* value, const interval_map& intervals) { return value->is<insn>() && intervals.contains(value->as<insn>()); }

		static bool dynamically_boxed_use(insn* user, size_t operand) {
			switch (user->opc) {
				case opcode::store_local:
					return operand == 1;
				case opcode::field_get:
					return operand >= 1;
				case opcode::field_set:
					return operand >= 1;
				case opcode::class_new:
				case opcode::class_is:
				case opcode::iter_next:
				case opcode::va_get:
				case opcode::coerce_bool:
				case opcode::test_type:
				case opcode::erase_type:
				case opcode::keep_alive:
				case opcode::release:
				case opcode::set_exception:
				case opcode::vcall:
				case opcode::ret:
					return true;
				case opcode::uval_set:
					return operand == 2;
				case opcode::extract:
					return operand == 0 && user->operands[1]->as<constant>()->i32 == 1;
				default:
					return false;
			}
		}

		static bool plan_boundaries(procedure* proc, const interval_map& intervals, type target, std::vector<boundary_use>& boundaries) {
			for (auto& block : proc->basic_blocks) {
				for (auto* user : block->insns()) {
					if (intervals.contains(user) || user->is<compare>())
						continue;
					std::vector<size_t> selected_operands;
					for (size_t index = 0; index != user->operands.size(); ++index)
						if (is_selected(user->operands[index].get(), intervals))
							selected_operands.emplace_back(index);
					if (selected_operands.empty())
						continue;

					if (user->is<binop>()) {
						bool retains_f64 = false;
						for (size_t index = 1; index != 3; ++index)
							retains_f64 |= !is_selected(user->operands[index].get(), intervals) && user->operands[index]->vt == type::f64;
						if (!retains_f64)
							boundaries.push_back({user, selected_operands.front(), boundary_kind::to_f64});
					} else if (user->is<unop>() || user->is<select>() || user->is<phi>() || user->is<move>() || user->is<retain>()) {
						for (auto operand : selected_operands)
							boundaries.push_back({user, operand, boundary_kind::to_f64});
					} else if (user->is<assume_cast>()) {
						if (user->operands[1]->as<constant>()->dty != type::f64)
							return false;
						boundaries.push_back({user, selected_operands.front(), boundary_kind::erase_only});
					} else if (user->is<ccall>()) {
						auto* info     = user->operands[0]->as<constant>()->nfni;
						auto  index    = user->operands[1]->as<constant>()->i32;
						auto& overload = info->overloads[index];
						for (auto operand : selected_operands) {
							if (operand < 2)
								return false;
							auto expected = overload.args[operand - 2];
							if (expected == type::f64)
								boundaries.push_back({user, operand, boundary_kind::to_f64});
							else if (expected != type::any && expected != target)
								return false;
						}
					} else {
						for (auto operand : selected_operands)
							if (!dynamically_boxed_use(user, operand))
								return false;
					}
					if (boundaries.size() > maximum_boundary_casts)
						return false;
				}
			}
			return true;
		}

		static void replace_exact_constant(procedure* proc, use<>& operand, type target) {
			auto value = exact_constant(operand.get());
			if (!value || value->minimum != value->maximum || !fits(target, *value))
				return;
			operand = target == type::i32 ? proc->add_const(constant{int32_t(value->minimum)}) : proc->add_const(constant{value->minimum});
		}

		static size_t specialize_loop(procedure* proc, const natural_loop& loop) {
			if (loop.over_budget || loop.blocks.size() > maximum_loop_blocks)
				return 0;

			std::vector<insn*> instructions;
			for (auto* block : loop.blocks) {
				for (auto* instruction : block->insns()) {
					if (instructions.size() == maximum_loop_instructions)
						return 0;
					instructions.emplace_back(instruction);
				}
			}

			std::optional<induction_proof> induction;
			for (auto* phi : loop.header->phis()) {
				auto proof = prove_induction(loop, phi);
				if (!proof)
					continue;
				if (induction)
					return 0;
				induction = std::move(proof);
			}
			if (!induction)
				return 0;

			interval_map intervals;
			intervals.reserve(maximum_candidates);
			if (!collect_intervals(loop, instructions, *induction, intervals))
				return 0;

			type target = type::i32;
			for (const auto& [instruction, value] : intervals) {
				if (!fits(type::i32, value)) {
					target = type::i64;
					break;
				}
				if (instruction->is<binop>() || instruction->is<phi>()) {
					size_t first_operand = instruction->is<binop>() ? 1 : 0;
					for (size_t index = first_operand; index != instruction->operands.size(); ++index) {
						auto operand = exact_constant(instruction->operands[index].get());
						if (operand && !fits(type::i32, *operand)) {
							target = type::i64;
							break;
						}
					}
					if (target == type::i64)
						break;
				}
			}

			std::vector<boundary_use> boundaries;
			if (!plan_boundaries(proc, intervals, target, boundaries))
				return 0;

			std::vector<insn*> comparisons;
			for (auto* instruction : instructions) {
				if (!instruction->is<compare>())
					continue;
				auto left  = interval_of(instruction->operands[1], intervals);
				auto right = interval_of(instruction->operands[2], intervals);
				if (!left || !right || !fits(target, *left) || !fits(target, *right))
					continue;
				if (is_selected(instruction->operands[1].get(), intervals) || is_selected(instruction->operands[2].get(), intervals))
					comparisons.emplace_back(instruction);
			}

			for (const auto& [instruction, value] : intervals) {
				(void) value;
				instruction->vt = target;
				if (instruction->is<binop>() || instruction->is<phi>()) {
					for (size_t index = instruction->is<binop>() ? 1 : 0; index != instruction->operands.size(); ++index)
						replace_exact_constant(proc, instruction->operands[index], target);
				}
			}
			for (auto* comparison : comparisons) {
				replace_exact_constant(proc, comparison->operands[1], target);
				replace_exact_constant(proc, comparison->operands[2], target);
				comparison->update();
			}

			std::unordered_set<insn*> touched_users;
			for (const auto& boundary : boundaries) {
				auto* source = boundary.user->operands[boundary.operand].get();
				auto* before = boundary.user;
				if (boundary.user->is<phi>())
					before = boundary.user->parent->predecessors[boundary.operand]->back();
				builder b{before};
				auto    erased = b.emit_before<erase_type>(before, source);
				if (boundary.kind == boundary_kind::erase_only) {
					boundary.user->operands[boundary.operand] = erased;
				} else {
					auto converted                            = b.emit_before<assume_cast>(before, erased, type::f64);
					boundary.user->operands[boundary.operand] = converted;
				}
				touched_users.emplace(boundary.user);
			}
			for (auto* user : touched_users)
				user->update();

			return intervals.size() + comparisons.size();
		}
	}

	size_t specialize_integer_ranges(procedure* proc) {
		size_t transformed = 0;
		for (const auto& loop : find_natural_loops(proc))
			transformed += specialize_loop(proc, loop);
		return transformed;
	}
};
