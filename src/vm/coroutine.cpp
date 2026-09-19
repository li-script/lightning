#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <thread>
#include <util/context.hpp>
#include <util/stack.hpp>
#include <util/user.hpp>
#include <utility>
#include <vm/array.hpp>
#include <vm/coroutine.hpp>
#include <vm/function.hpp>
#include <vm/iterator.hpp>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>

namespace li {
	static_assert(coroutine_native_stack_headroom < coroutine_native_stack_initial_floor);
	static_assert(coroutine_native_stack_initial_floor <= coroutine_native_stack_reservation);

	struct coroutine_state {
		vm*                               owner = nullptr;
		std::thread::id                   owner_thread{};
		std::unique_ptr<any[]>            vm_stack{};
		platform::native_stack            native_stack{};
		platform::native_context          native_context{};
		platform::sanitizer_fiber_context sanitizer_fiber{};
		vm_execution_state                execution{};
		function*                         entry          = nullptr;
		coroutine_state*                  resumer        = nullptr;
		bool                              resumer_active = false;
		std::array<any, MAX_ARGS>         pending{};
		slot_t                            pending_count   = 0;
		any                               outgoing        = nil;
		coroutine_status                  status          = coroutine_status::created;
		bool                              close_requested = false;

		coroutine_state(vm* L, function* fn)
			 : owner(L),
				owner_thread(std::this_thread::get_id()),
				vm_stack(std::make_unique<any[]>(STACK_LENGTH)),
				native_stack(coroutine_native_stack_reservation),
				entry(fn) {
			if (native_stack.committed_size() < coroutine_native_stack_initial_floor && !native_stack.grow(coroutine_native_stack_initial_floor))
				throw std::bad_alloc();

			execution.stack              = vm_stack.get();
			execution.stack_top          = execution.stack;
			execution.stack_limit        = execution.stack + STACK_LENGTH;
			sanitizer_fiber.stack_bottom = native_stack.data();
			sanitizer_fiber.stack_size   = native_stack.committed_size();
			rc::retain(entry);
		}

		~coroutine_state() {
			clear_pending();
			rc::clear(owner, outgoing);
			rc::clear(owner, execution.last_ex);
			rc::replace(owner, execution.last_exception_trace, static_cast<array*>(nullptr));
			rc::replace(owner, entry, static_cast<function*>(nullptr));

			any* top = std::exchange(execution.stack_top, execution.stack);
			while (top && top != execution.stack) {
				any value = *--top;
				*top      = nil;
				rc::release(owner, value);
			}
		}

		void clear_pending() {
			while (pending_count) {
				any& value = pending[--pending_count];
				rc::clear(owner, value);
			}
		}

		bool set_pending(any* args, slot_t count, slot_t skip) {
			clear_pending();
			for (slot_t i = 0; i != count; ++i) {
				any value = args[-(i + skip)];
				if (!rc::try_retain(owner, value)) {
					clear_pending();
					return false;
				}
				pending[pending_count++] = value;
			}
			return true;
		}

		any take_outgoing() { return std::exchange(outgoing, nil); }

		bool set_outgoing(any_t value) { return rc::try_replace(owner, outgoing, value); }

		void adopt_outgoing(any value) {
			rc::clear(owner, outgoing);
			outgoing = value;
		}
	};

	namespace {
		constexpr bool                                 context_is_supported = platform::native_context_supported;
		thread_local platform::sanitizer_fiber_context main_sanitizer_fiber{};

		coroutine_state* state_from_object(object* value) noexcept {
			const auto base    = reinterpret_cast<std::uintptr_t>(value->context);
			const auto aligned = (base + alignof(coroutine_state) - 1) & ~(std::uintptr_t(alignof(coroutine_state)) - 1);
			return reinterpret_cast<coroutine_state*>(aligned);
		}

		const coroutine_state* state_from_object(const object* value) noexcept { return state_from_object(const_cast<object*>(value)); }

		coroutine_state* state_from_value(const vm* L, any_t value) noexcept {
			if (!L || !value.is_obj() || !L->coroutine_class)
				return nullptr;
			object* instance = value.as_obj();
			if (instance->cl != L->coroutine_class || !instance->gc_hook)
				return nullptr;
			return state_from_object(instance);
		}

		vm_execution_state& saved_execution(vm* L, coroutine_state* value) noexcept { return value ? value->execution : L->main_execution; }

		platform::native_context& saved_native_context(vm* L, coroutine_state* value) noexcept { return value ? value->native_context : L->main_context; }

		platform::sanitizer_fiber_context& saved_sanitizer_fiber(coroutine_state* value) noexcept {
			return value ? value->sanitizer_fiber : main_sanitizer_fiber;
		}

		void save_current_execution(vm* L, vm_execution_state& destination) noexcept {
			destination.stack                = L->stack;
			destination.stack_top            = L->stack_top;
			destination.stack_limit          = L->stack_limit;
			destination.last_ex              = std::exchange(L->last_ex, nil);
			destination.last_exception_trace = std::exchange(L->last_exception_trace, nullptr);
			destination.last_vm_caller       = L->last_vm_caller;
			destination.exception_handler    = L->exception_handler;
			destination.cleanup_handler      = L->cleanup_handler;
			destination.cleanup_depth        = L->cleanup_depth;
		}

		void restore_execution(vm* L, vm_execution_state& source) noexcept {
			L->stack                = source.stack;
			L->stack_top            = source.stack_top;
			L->stack_limit          = source.stack_limit;
			L->last_ex              = std::exchange(source.last_ex, nil);
			L->last_exception_trace = std::exchange(source.last_exception_trace, nullptr);
			L->last_vm_caller       = source.last_vm_caller;
			L->exception_handler    = source.exception_handler;
			L->cleanup_handler      = source.cleanup_handler;
			L->cleanup_depth        = source.cleanup_depth;
		}

		void switch_execution(vm* L, coroutine_state* target, bool source_finished = false) noexcept {
			coroutine_state* source = L->current_coroutine;
			if (source == target)
				std::terminate();

			auto& source_context = saved_native_context(L, source);
			auto& target_context = saved_native_context(L, target);
			save_current_execution(L, saved_execution(L, source));
			restore_execution(L, saved_execution(L, target));
			L->current_coroutine = target;
			std::atomic_signal_fence(std::memory_order_seq_cst);
#if LI_CONTEXT_SUPPORTED
			auto& source_sanitizer = saved_sanitizer_fiber(source);
			auto& target_sanitizer = saved_sanitizer_fiber(target);
			platform::start_sanitizer_fiber_switch(source_sanitizer, target_sanitizer, source_finished);
			platform::context_switch(&source_context, &target_context);
			platform::finish_sanitizer_fiber_switch();
#else
			(void) source_context;
			(void) target_context;
			std::terminate();
#endif
			std::atomic_signal_fence(std::memory_order_seq_cst);
			if (L->current_coroutine != source)
				std::terminate();
		}

		std::uintptr_t saved_stack_pointer(const platform::native_context& context) noexcept {
#if LI_ARCH_ARM && !LI_32 && !LI_WINDOWS
			return context.sp;
#elif LI_ARCH_X86 && !LI_32 && LI_ABI_SYSV64
			return context.rsp;
#elif LI_ARCH_X86 && !LI_32 && LI_ABI_MS64
			return context.rsp;
#else
			(void) context;
			return 0;
#endif
		}

#if LI_CONTEXT_SUPPORTED
		bool has_native_stack_headroom(std::uintptr_t stack_pointer, platform::native_stack_bounds bounds, std::size_t required_bytes) noexcept {
			if (!bounds || stack_pointer < bounds.low || stack_pointer > bounds.high)
				return false;
			if (required_bytes > std::numeric_limits<std::size_t>::max() - coroutine_native_stack_headroom)
				return false;
			const std::size_t total_required = coroutine_native_stack_headroom + required_bytes;
			return stack_pointer - bounds.low >= total_required;
		}
#endif

		bool prepare_native_stack(coroutine_state& value) noexcept {
			const auto stack_pointer = saved_stack_pointer(value.native_context);
			const auto committed_low = reinterpret_cast<std::uintptr_t>(value.native_stack.data());
			if (!stack_pointer || stack_pointer < committed_low)
				return false;
			if ((stack_pointer - committed_low) >= coroutine_native_stack_headroom)
				return true;

			const std::size_t committed = value.native_stack.committed_size();
			const std::size_t reserved  = value.native_stack.reserved_size();
			const std::size_t desired   = std::min(reserved, std::max(committed * 2, committed + coroutine_native_stack_headroom));
			if (desired <= committed || !value.native_stack.grow(desired))
				return false;
			value.sanitizer_fiber.stack_bottom = value.native_stack.data();
			value.sanitizer_fiber.stack_size   = value.native_stack.committed_size();
#if LI_CONTEXT_SUPPORTED
			platform::set_context_stack_bounds(value.native_context, value.native_stack.data(), value.native_stack.end());
#endif
			return true;
		}

		any take_resume_value(coroutine_state& value) {
			if (!value.pending_count)
				return nil;
			if (value.pending_count == 1) {
				value.pending_count = 0;
				return std::exchange(value.pending[0], nil);
			}

			array* result = array::create(value.owner, 0, msize_t(value.pending_count));
			for (slot_t i = 0; i != value.pending_count; ++i) {
				result->push(value.owner, value.pending[i]);
				rc::clear(value.owner, value.pending[i]);
			}
			value.pending_count = 0;
			return any(result);
		}

		void retire_dead(coroutine_state& value, bool release_exception) {
			value.clear_pending();
			rc::replace(value.owner, value.entry, static_cast<function*>(nullptr));
			if (release_exception) {
				rc::clear(value.owner, value.execution.last_ex);
				rc::replace(value.owner, value.execution.last_exception_trace, static_cast<array*>(nullptr));
			}
			value.sanitizer_fiber = {};
			value.native_stack    = platform::native_stack{};
		}

		void coroutine_entry(void* opaque) {
			auto& value = *static_cast<coroutine_state*>(opaque);
			vm*   L     = value.owner;
			if (L->current_coroutine != &value)
				std::terminate();

			const slot_t count = value.pending_count;
			if (!L->vm_stack_headroom_available(size_t(count) + FRAME_SIZE)) [[unlikely]] {
				value.clear_pending();
				value.adopt_outgoing(L->vm_stack_exhausted());
				return;
			}
			for (slot_t i = count; i != 0; --i)
				L->push_stack(value.pending[i - 1]);
			value.clear_pending();

			try {
				value.adopt_outgoing(L->call(count, any(value.entry)));
			} catch (...) {
				L->truncate_stack(L->stack);
				L->clear_exception();
				value.adopt_outgoing(exception_marker);
			}
		}

		[[noreturn]] void coroutine_completion(void* opaque) {
			auto& value = *static_cast<coroutine_state*>(opaque);
			vm*   L     = value.owner;
			if (L->current_coroutine != &value || !value.resumer_active)
				std::terminate();

			value.status            = coroutine_status::dead;
			coroutine_state* target = value.resumer;
			value.resumer           = nullptr;
			value.resumer_active    = false;
			switch_execution(L, target, true);
			std::terminate();
		}

		struct receiver_call {
			coroutine_state* state = nullptr;
			any*             args  = nullptr;
			slot_t           count = 0;
			slot_t           skip  = 0;
		};

		bool decode_receiver(vm* L, any* args, slot_t n, receiver_call& result) {
			any receiver = args[1];
			if (coroutine_state* state = state_from_value(L, receiver)) {
				result = {state, args, n, 0};
				return true;
			}
			if (n) {
				if (coroutine_state* state = state_from_value(L, args[0])) {
					result = {state, args, n - 1, 1};
					return true;
				}
			}
			L->error("expected coroutine receiver");
			return false;
		}

		any_t resume_coroutine(vm* L, receiver_call call) {
			coroutine_state& value = *call.state;
			if (value.owner != L)
				return L->error("coroutine belongs to another VM");
			if (value.owner_thread != std::this_thread::get_id())
				return L->error("coroutine cannot be resumed from another thread");
			if (L->current_coroutine == &value || value.status == coroutine_status::running)
				return L->error("coroutine is already running");
			if (value.status == coroutine_status::dead)
				return L->error("cannot resume dead coroutine");
			if (L->active_locks)
				return L->error("cannot resume a coroutine while a shared lock is active");
			if (L->gc.finalizer_context)
				return L->error("cannot resume a coroutine during object finalization");
			if (!prepare_native_stack(value))
				return L->error("unable to provide coroutine native-stack headroom");

			if (!value.set_pending(call.args, call.count, call.skip))
				return exception_marker;
			value.resumer        = L->current_coroutine;
			value.resumer_active = true;
			value.status         = coroutine_status::running;
			switch_execution(L, &value);
			value.resumer        = nullptr;
			value.resumer_active = false;

			if (value.status == coroutine_status::suspended)
				return L->take(value.take_outgoing());
			if (value.status != coroutine_status::dead)
				return L->error("coroutine returned in an invalid state");

			any result = value.take_outgoing();
			if (result.is_exc()) {
				any    exception = std::exchange(value.execution.last_ex, nil);
				array* trace     = std::exchange(value.execution.last_exception_trace, nullptr);
				retire_dead(value, false);
				rc::replace_adopt(L, L->last_ex, exception);
				array* previous_trace   = L->last_exception_trace;
				L->last_exception_trace = trace;
				rc::release(L, previous_trace);
				return exception_marker;
			}
			retire_dead(value, true);
			return L->take(result);
		}

		bool force_close(vm* L, coroutine_state& value, bool report_error, bool allow_during_finalizer = false) {
			auto fail = [&](const char* message) {
				if (report_error)
					L->error(message);
				return false;
			};

			if (value.owner != L)
				return fail("coroutine belongs to another VM");
			if (value.owner_thread != std::this_thread::get_id())
				return fail("coroutine cannot be closed from another thread");
			if (L->current_coroutine == &value || value.status == coroutine_status::running)
				return fail("cannot close a running coroutine");
			if (L->active_locks)
				return fail("cannot close a coroutine while a shared lock is active");
			if (value.status == coroutine_status::dead)
				return true;
			if (value.status == coroutine_status::suspended && L->gc.finalizer_context && !allow_during_finalizer)
				return fail("cannot close a suspended coroutine during object finalization");

			value.close_requested = true;
			value.clear_pending();
			rc::clear(L, value.outgoing);
			if (value.status == coroutine_status::created) {
				value.status = coroutine_status::dead;
				retire_dead(value, true);
				return true;
			}

			value.resumer        = L->current_coroutine;
			value.resumer_active = true;
			value.status         = coroutine_status::running;
			switch_execution(L, &value);
			value.resumer        = nullptr;
			value.resumer_active = false;
			if (value.status != coroutine_status::dead)
				return fail("coroutine refused forced unwind");

			rc::clear(L, value.outgoing);
			retire_dead(value, true);
			return true;
		}

		void coroutine_gc_hook(object* instance) {
			coroutine_state* value = state_from_object(instance);
			if (value->status != coroutine_status::dead && !force_close(value->owner, *value, false, true))
				value->owner->panic("unable to safely destroy a live coroutine");
			std::destroy_at(value);
		}

		any_t LI_CC native_create(vm* L, any* args, slot_t n) {
			if (n != 1 || !args[0].is_fn())
				return L->error("coroutine.create expects one function");
			if (!context_is_supported)
				return L->error("stackful coroutines are unsupported on this target");
			if (!L->coroutine_class)
				return L->error("coroutine library is not registered");
			if (!rc::check_store(L, args[0]))
				return exception_marker;

			object*          instance = object::create(L, L->coroutine_class);
			coroutine_state* value    = state_from_object(instance);
			try {
				std::construct_at(value, L, args[0].as_fn());
			} catch (...) {
				rc::release(L, instance);
				return L->error("failed to allocate coroutine stacks");
			}
			instance->gc_hook = &coroutine_gc_hook;
#if LI_CONTEXT_SUPPORTED
			platform::make_context(value->native_context, value->native_stack.end(), &coroutine_entry, value, &coroutine_completion, value);
			platform::set_context_stack_bounds(value->native_context, value->native_stack.data(), value->native_stack.end());
#endif
			return L->take(instance);
		}

		any_t LI_CC native_resume(vm* L, any* args, slot_t n) {
			receiver_call call{};
			if (!decode_receiver(L, args, n, call))
				return exception_marker;
			return resume_coroutine(L, call);
		}

		any_t LI_CC native_yield(vm* L, any* args, slot_t n) {
			if (n > 1)
				return L->error("coroutine.yield expects zero or one value");
			coroutine_state* value = L->current_coroutine;
			if (!value)
				return L->error("cannot yield outside a coroutine");
			if (value->close_requested)
				return exception_marker;
			if (L->active_locks)
				return L->error("cannot yield while a shared lock is active");
			if (L->gc.finalizer_context)
				return L->error("cannot yield during object finalization");
			if (!value->resumer_active)
				return L->error("coroutine has no active resumer");

			if (!(n ? value->set_outgoing(args[0]) : value->set_outgoing(nil)))
				return exception_marker;
			value->status           = coroutine_status::suspended;
			coroutine_state* target = value->resumer;
			switch_execution(L, target);

			if (value->close_requested)
				return exception_marker;
			return L->take(take_resume_value(*value));
		}

		any_t LI_CC native_status(vm* L, any* args, slot_t n) {
			receiver_call call{};
			if (!decode_receiver(L, args, n, call))
				return exception_marker;
			if (call.count)
				return L->error("coroutine.status expects no arguments");

			std::string_view name;
			switch (call.state->status) {
				case coroutine_status::created:
					name = "created";
					break;
				case coroutine_status::running:
					name = "running";
					break;
				case coroutine_status::suspended:
					name = "suspended";
					break;
				case coroutine_status::dead:
					name = "dead";
					break;
			}
			return L->take(string::create(L, name));
		}

		any_t LI_CC native_close(vm* L, any* args, slot_t n) {
			receiver_call call{};
			if (!decode_receiver(L, args, n, call))
				return exception_marker;
			if (call.count)
				return L->error("coroutine.close expects no arguments");
			if (!force_close(L, *call.state, true))
				return exception_marker;
			return L->ok();
		}

		any_t LI_CC native_enter_cleanup(vm* L, any*, slot_t n) {
			if (n)
				return L->error("cleanup entry helper expects no arguments");
			if (L->cleanup_depth == std::numeric_limits<uint32_t>::max())
				return L->error("cleanup nesting is too deep");
			++L->cleanup_depth;
			return L->ok();
		}

		any_t LI_CC native_leave_cleanup(vm* L, any*, slot_t n) {
			if (n)
				return L->error("cleanup exit helper expects no arguments");
			if (!L->cleanup_depth)
				return L->error("cleanup exit without matching entry");
			--L->cleanup_depth;
			return L->ok();
		}

		any_t LI_CC native_next(vm* L, any* args, slot_t n) {
			receiver_call call{};
			if (!decode_receiver(L, args, n, call))
				return exception_marker;

			if (call.state->status == coroutine_status::dead) {
				array* pair = array::create(L, 0, 2);
				pair->push(L, nil);
				pair->push(L, const_true);
				return L->take(pair);
			}

			any result = resume_coroutine(L, call);
			if (result.is_exc())
				return result;
			const bool done = call.state->status == coroutine_status::dead;
			array*     pair = array::create(L, 0, 2);
			pair->push(L, result);
			pair->push(L, any(done));
			rc::release(L, result);
			return L->take(pair);
		}
	}

	next_result coroutine_next(vm* L, any_t value) {
		coroutine_state* state = state_from_value(L, value);
		if (!state) {
			L->error("expected coroutine iterator");
			return next_result{.ok = false};
		}
		if (state->status == coroutine_status::dead)
			return next_result{.value = nil, .done = true, .ok = true};

		receiver_call call{.state = state, .args = nullptr, .count = 0, .skip = 0};
		any           result = resume_coroutine(L, call);
		if (result.is_exc())
			return next_result{.ok = false};
		return next_result{
			 .value = result,
			 .done  = state->status == coroutine_status::dead,
			 .ok    = true,
		};
	}

	bool is_coroutine(const vm* L, any_t value) noexcept { return state_from_value(L, value) != nullptr; }

	coroutine_status get_coroutine_status(const vm* L, any_t value) noexcept {
		if (const coroutine_state* state = state_from_value(L, value))
			return state->status;
		return coroutine_status::dead;
	}

	bool vm::forced_unwind_pending() const noexcept { return current_coroutine && current_coroutine->close_requested; }

	bool vm::forced_unwind_requested() const noexcept { return forced_unwind_pending() && cleanup_depth == 0; }

	bool vm::native_stack_headroom_available(std::size_t required_bytes) const noexcept {
		if (required_bytes > std::numeric_limits<std::size_t>::max() - coroutine_native_stack_headroom)
			return false;
#if LI_CONTEXT_SUPPORTED
		const auto                    stack_pointer = platform::current_stack_pointer();
		platform::native_stack_bounds bounds;
		if (current_coroutine) {
			bounds.low  = reinterpret_cast<std::uintptr_t>(current_coroutine->native_stack.data());
			bounds.high = reinterpret_cast<std::uintptr_t>(current_coroutine->native_stack.end());
		} else {
			bounds = platform::current_native_stack_bounds();
		}
		return has_native_stack_headroom(stack_pointer, bounds, required_bytes);
#elif LI_EMSCRIPTEN
		// Emscripten owns the checked linear stack and exposes no native OS interval.
		// Preserve that existing platform fallback explicitly.
		return true;
#else
		return false;
#endif
	}

	void lib::register_coroutine(vm* L) {
		if (L->coroutine_class)
			return;

		function* create_function        = function::create(L, &native_create);
		function* resume_function        = function::create(L, &native_resume);
		function* yield_function         = function::create(L, &native_yield);
		function* status_function        = function::create(L, &native_status);
		function* close_function         = function::create(L, &native_close);
		function* next_function          = function::create(L, &native_next);
		function* enter_cleanup_function = function::create(L, &native_enter_cleanup);
		function* leave_cleanup_function = function::create(L, &native_leave_cleanup);

		constexpr std::size_t                 method_count = 4;
		std::array<string*, method_count + 1> keys         = {
			 string::create(L, "resume"),
			 string::create(L, "status"),
			 string::create(L, "close"),
			 string::create(L, "next"),
			 string::create(L, "@coroutine-state"),
		};
		std::array<function*, method_count> methods = {
			 resume_function,
			 status_function,
			 close_function,
			 next_function,
		};
		std::array<field_pair, method_count + 1>        fields{};
		std::array<uint8_t, method_count * sizeof(any)> static_values{};
		for (std::size_t i = 0; i != method_count; ++i) {
			fields[i] = field_pair{
				 .key = keys[i],
				 .value =
					  field_info{
							.ty        = type::any,
							.offset    = uint32_t(i * sizeof(any)),
							.is_static = true,
					  },
			};
			any(methods[i]).store_at(static_values.data() + i * sizeof(any), type::any);
		}

		constexpr std::size_t object_storage_size = sizeof(coroutine_state) + alignof(coroutine_state) - 1;
		fields[method_count]                      = field_pair{
			 .key = keys[method_count],
			 .value =
				  field_info{
						.ty     = type::exc,
						.offset = uint32_t(object_storage_size - sizeof(any)),
				  },
		};
		std::array<uint8_t, object_storage_size> default_values{};
		string*                                  class_name = string::create(L, "coroutine");
		vclass* coroutine_class  = vclass::create(L, class_name, fields, default_values, static_values, nullptr, reserve_class_identity(L));
		coroutine_class->cxx_tid = util::type_id_v<coroutine_state>;
		L->coroutine_class       = coroutine_class;

		util::export_as(L, "coroutine.create", any(create_function));
		util::export_as(L, "coroutine.resume", any(resume_function));
		util::export_as(L, "coroutine.yield", any(yield_function));
		util::export_as(L, "coroutine.status", any(status_function));
		util::export_as(L, "coroutine.close", any(close_function));
		util::export_as(L, "coroutine.next", any(next_function));
		util::export_as(L, "coroutine.@enter_cleanup", any(enter_cleanup_function));
		util::export_as(L, "coroutine.@leave_cleanup", any(leave_cleanup_function));

		rc::release(L, class_name);
		for (string* key : keys)
			rc::release(L, key);
		rc::release(L, create_function);
		rc::release(L, resume_function);
		rc::release(L, yield_function);
		rc::release(L, status_function);
		rc::release(L, close_function);
		rc::release(L, next_function);
		rc::release(L, enter_cleanup_function);
		rc::release(L, leave_cleanup_function);
	}
}
