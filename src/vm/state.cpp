#include <lib/std.hpp>
#include <limits>
#include <utility>
#include <vm/array.hpp>
#include <vm/function.hpp>
#include <vm/object.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>

namespace li {
	vm* vm::create(fn_alloc allocate, void* allocation_context) {
		if (!allocate)
			return nullptr;

		constexpr size_t stack_bytes = size_t(STACK_LENGTH) * sizeof(any);
		if (stack_bytes > (std::numeric_limits<size_t>::max() - sizeof(vm)))
			return nullptr;
		size_t vm_bytes = sizeof(vm) + stack_bytes;
		if (vm_bytes > (std::numeric_limits<size_t>::max() - (gc::chunk_size - 1)))
			return nullptr;
		size_t page_overhead = gc::chunk_ceil(sizeof(gc::page));
		size_t object_bytes  = gc::chunk_ceil(vm_bytes);
		if (object_bytes > (std::numeric_limits<size_t>::max() - page_overhead))
			return nullptr;
		size_t allocation_bytes = std::max(gc::minimum_allocation, page_overhead + object_bytes);
		if (allocation_bytes > (std::numeric_limits<size_t>::max() - 0xFFF))
			return nullptr;
		size_t           page_count         = (allocation_bytes + 0xFFF) >> 12;
		constexpr size_t maximum_page_count = size_t(std::numeric_limits<msize_t>::max()) >> (12 - gc::chunk_shift);
		if (!page_count || page_count > maximum_page_count)
			return nullptr;

		void* storage = allocate(allocation_context, nullptr, page_count, false);
		if (!storage)
			return nullptr;

		gc::state allocator{};
		allocator.alloc_fn     = allocate;
		allocator.alloc_ctx    = allocation_context;
		allocator.initial_page = new (storage) gc::page(page_count);

		vm* L = allocator.create<vm>(nullptr, stack_bytes);
		if (!L) {
			allocate(allocation_context, storage, page_count, false);
			allocate(allocation_context, allocation_context, 0, false);
			return nullptr;
		}
		L->gc          = std::move(allocator);
		L->stack       = L->main_stack;
		L->stack_top   = L->main_stack;
		L->stack_limit = L->main_stack + STACK_LENGTH;

		L->modules            = table::create(L, 32);
		L->modules->is_frozen = true;
		strset_init(L);
		typeset_init(L);
		lib::detail::register_builtin(L);
		lib::detail::register_math(L);
		return L;
	}

	any_t vm::vm_stack_exhausted() { return error("VM stack exhausted while entering script frame"); }

	void LI_CC exception_location(vm* L, int32_t bytecode_pc) {
		if (!L || L->last_exception_trace || bytecode_pc < 0 || uint32_t(bytecode_pc) > BC_MAX_IP)
			return;
		const slot_t top = slot_t(L->stack_top - L->stack);
		for (slot_t caller_slot = top - 1; caller_slot >= FRAME_SIZE - 1; --caller_slot) {
			any target = L->stack[caller_slot - 1];
			if (!target.is_fn() || !target.as_fn()->is_virtual())
				continue;
			call_frame   caller = li::bit_cast<call_frame>(L->stack[caller_slot].value);
			const slot_t local0 = caller_slot + 1;
			if (caller.stack_pos >= msize_t(local0) || local0 + slot_t(target.as_fn()->proto->num_locals) != top)
				continue;
			L->capture_exception_trace(target.as_fn(), uint32_t(bytecode_pc), L->stack + local0);
			return;
		}
	}

	const nfunc_info exception_location_info = {
		 func_attr_sideeffect | func_attr_c_takes_vm,
		 "vm.exception_location",
		 {nfunc_overload{li::bit_cast<const void*>(&exception_location), {type::i32}, type::nil}},
	};

	namespace {
		msize_t frame_line(vm* L, call_frame frame) {
			if (frame.stack_pos < FRAME_SIZE || frame.stack_pos >= msize_t(L->stack_top - L->stack))
				return 0;
			any target = L->stack[frame.stack_pos + FRAME_TARGET];
			if (!target.is_fn() || !target.as_fn()->is_virtual())
				return 0;
			return target.as_fn()->proto->lookup_line(frame.caller_pc & ~FRAME_C_FLAG);
		}

		function* find_native_frame(vm* L, call_frame caller, slot_t upper) {
			if (upper > L->stack_top - L->stack)
				upper = slot_t(L->stack_top - L->stack);
			const uint64_t expected = li::bit_cast<uint64_t>(call_frame{
				 .caller_pc = caller.caller_pc & ~FRAME_C_FLAG,
				 .stack_pos = caller.stack_pos,
			});
			for (slot_t caller_slot = upper - 1; caller_slot > slot_t(caller.stack_pos); --caller_slot) {
				if (L->stack[caller_slot].value != expected)
					continue;
				any target = L->stack[caller_slot - 1];
				if (target.is_fn() && target.as_fn()->is_native())
					return target.as_fn();
			}
			return nullptr;
		}
	}

	any_t vm::set_exception(any result, function* origin, uint32_t origin_pc, any* origin_local0) {
		if (last_exception_trace && result == last_ex)
			return ok();
		if (!rc::try_retain(this, result))
			return exception_marker;
		rc::replace(this, last_exception_trace, static_cast<array*>(nullptr));
		rc::replace_adopt(this, last_ex, result);
		capture_exception_trace(origin, origin_pc, origin_local0);
		return ok();
	}

	any_t vm::adopt_exception(any result, function* origin, uint32_t origin_pc, any* origin_local0) {
		rc::replace(this, last_exception_trace, static_cast<array*>(nullptr));
		rc::replace_adopt(this, last_ex, result);
		capture_exception_trace(origin, origin_pc, origin_local0);
		return exception_marker;
	}

	void vm::capture_exception_trace(function* origin, uint32_t origin_pc, any* origin_local0) {
		if (last_exception_trace || gc.destroying)
			return;

		array*  trace        = array::create(this, 0, 8);
		string* function_key = string::create(this, "function");
		string* line_key     = string::create(this, "line");
		string* native_key   = string::create(this, "native");
		auto    append       = [&](function* target, msize_t line, bool native) {
			if (!target)
				return;
			string* name = nullptr;
			if (native) {
				const char* native_name = target->ninfo && target->ninfo->name ? target->ninfo->name : "C";
				name                    = string::create(this, native_name);
			} else {
				if (!target->proto || !target->proto->src_chunk || !rc::try_retain(this, target->proto->src_chunk))
					return;
				name = target->proto->src_chunk;
			}
			table* entry  = table::create(this, 3);
			bool   stored = entry->set(this, any(function_key), any(name)) && entry->set(this, any(line_key), any(number(line))) &&
								 entry->set(this, any(native_key), any(native)) && trace->push(this, any(entry));
			rc::release(this, name);
			rc::release(this, entry);
			if (!stored)
				panic("failed to record exception trace");
		};

		slot_t upper    = slot_t(stack_top - stack);
		bool   can_walk = origin != nullptr;
		if (origin) {
			msize_t line = 0;
			if (origin->is_virtual() && origin_pc != UINT32_MAX && origin_pc < origin->proto->length)
				line = origin->proto->lookup_line(origin_pc);
			append(origin, line, origin->is_native());
			if (origin_local0 >= stack && origin_local0 <= stack_top)
				upper = slot_t(origin_local0 - stack);
		}

		call_frame frame = last_vm_caller;
		if (origin_local0 && origin_local0 > stack && origin_local0 <= stack_top) {
			frame = li::bit_cast<call_frame>(origin_local0[FRAME_CALLER].value);
		} else if (function* native = find_native_frame(this, frame, upper)) {
			append(native, frame_line(this, frame), true);
			can_walk = true;
		}

		while (can_walk && frame.stack_pos >= FRAME_SIZE && frame.stack_pos < msize_t(stack_top - stack)) {
			if (frame.multiplexed_by_c()) {
				if (function* native = find_native_frame(this, frame, upper))
					append(native, frame_line(this, frame), true);
			}

			any target = stack[frame.stack_pos + FRAME_TARGET];
			if (target.is_fn())
				append(target.as_fn(), frame_line(this, frame), target.as_fn()->is_native());

			call_frame next = li::bit_cast<call_frame>(stack[frame.stack_pos + FRAME_CALLER].value);
			if (next.stack_pos >= frame.stack_pos)
				break;
			upper = slot_t(frame.stack_pos);
			frame = next;
		}

		rc::release(this, function_key);
		rc::release(this, line_key);
		rc::release(this, native_key);
		if (trace->length)
			last_exception_trace = trace;
		else
			rc::release(this, trace);
	}

	array* vm::copy_exception_trace(any error) {
		array* result = array::create(this, 0, last_exception_trace ? last_exception_trace->length : 0);
		if (!last_exception_trace || error != last_ex)
			return result;
		for (any value : *last_exception_trace) {
			if (!value.is_tbl())
				continue;
			table* entry = value.as_tbl()->duplicate(this);
			if (!result->push(this, any(entry))) {
				rc::release(this, entry);
				rc::release(this, result);
				return nullptr;
			}
			rc::release(this, entry);
		}
		return result;
	}

	void vm::clear_exception() {
		rc::clear(this, last_ex);
		rc::replace(this, last_exception_trace, static_cast<array*>(nullptr));
	}

	std::string vm::format_exception_trace() {
		std::string result;
		if (!last_exception_trace)
			return result;
		for (any value : *last_exception_trace) {
			if (!value.is_tbl())
				continue;
			any     name   = nil;
			msize_t line   = 0;
			bool    native = false;
			for (auto [key, field] : *value.as_tbl()) {
				if (!key.is_str())
					continue;
				auto key_name = key.as_str()->view();
				if (key_name == "function")
					name = field;
				else if (key_name == "line" && field.is_num())
					line = msize_t(field.as_num());
				else if (key_name == "native")
					native = field.coerce_bool();
			}
			result += "  at ";
			result += name.is_str() ? name.as_str()->c_str() : "?";
			if (line) {
				result += ':';
				result += std::to_string(line);
			}
			if (native)
				result += " [native]";
			result += '\n';
		}
		return result;
	}

	void vm::truncate_stack(any* end) {
		if (end < stack || end > stack_top || stack_top > stack_limit) [[unlikely]]
			panic("invalid stack truncation.");

		any* previous_top = stack_top;
		stack_top         = end;
		while (previous_top != end) {
			any* slot  = --previous_top;
			any  value = *slot;
			*slot      = nil;
			rc::release(this, value);
		}
	}

	void vm::close() {
		if (current_coroutine) [[unlikely]]
			panic("closing VM from a running coroutine");
		rc::replace(this, coroutine_class, static_cast<vclass*>(nullptr));
		rc::replace(this, iterator_class, static_cast<vclass*>(nullptr));
		rc::replace(this, range_class, static_cast<vclass*>(nullptr));
		gc.close(this);
	}
}
