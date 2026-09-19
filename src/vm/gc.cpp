#include <atomic>
#include <cmath>
#include <cstring>
#include <utility>
#include <vm/array.hpp>
#include <vm/gc.hpp>
#include <vm/rc.hpp>
#include <vm/shared.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/traits.hpp>
#include <vm/weak.hpp>

namespace li::gc {
	static constexpr uint32_t small_class_count   = 8;
	static constexpr uint32_t max_realistic_alloc = 2 * 1024 * 1024 / chunk_size;

	static int size_class_of(uint32_t chunks) {
		if (chunks <= small_class_count)
			return int(chunks - 1);
		chunks -= small_class_count;
		float ratio = std::min(1.0f, std::sqrt(float(chunks) / float(max_realistic_alloc)));
		return int(small_class_count + std::floor((num_size_classes - small_class_count - 1) * ratio));
	}

	void header::initialize(page* owner, msize_t chunks, value_type type) {
		shared      = 0;
		is_static   = 0;
		page_offset = uint32_t((uintptr_t(this) - uintptr_t(owner)) >> 12);
		num_chunks  = chunks;
		type_id     = type;
		refcount    = 1;
	}

	std::pair<page*, header*> state::allocate_uninit(vm* L, msize_t chunks) {
		LI_ASSERT(chunks != 0);
		if (closing)
			return {nullptr, nullptr};

		if (chunks <= small_class_count) [[likely]] {
			auto& exact = free_lists[chunks - 1];
			if (exact) [[likely]] {
				header* result  = std::exchange(exact, exact->get_next_free());
				page*   owner   = result->get_page();
				result->type_id = type_invalid;
				owner->num_objects++;
				return {owner, result};
			}
		}

		for (int class_index = size_class_of(chunks); class_index < int(num_size_classes); class_index++) {
			header** link = &free_lists[class_index];
			while (header* candidate = *link) {
				if (candidate->num_chunks < chunks) {
					link = &candidate->ref_next_free();
					continue;
				}

				*link       = candidate->get_next_free();
				page* owner = candidate->get_page();
				owner->num_objects++;
				candidate->type_id = type_invalid;

				if (msize_t leftover = candidate->num_chunks - chunks) {
					candidate->num_chunks  = chunks;
					header* remainder      = candidate->next();
					remainder->shared      = 0;
					remainder->is_static   = 0;
					remainder->page_offset = uint32_t((uintptr_t(remainder) - uintptr_t(owner)) >> 12);
					remainder->num_chunks  = leftover;
					remainder->type_id     = type_gc_free;
					remainder->refcount    = 0;
					auto& remainder_list   = free_lists[size_class_of(leftover)];
					remainder->set_next_free(remainder_list);
					remainder_list = remainder;
				}
				return {owner, candidate};
			}
		}

		page* owner = initial_page->next;
		if (!owner->check_space(chunks)) {
			owner = add_page(L, size_t(chunks) << chunk_shift);
			if (!owner)
				return {nullptr, nullptr};
		}
		return {owner, owner->alloc_arena(chunks)};
	}

	void state::free_storage(header* value) {
		LI_ASSERT(value && !value->is_static && !value->is_free());
		LI_ASSERT(value->shared ? std::atomic_ref<uint32_t>(value->refcount).load(std::memory_order_relaxed) == destroying_refcount
										: value->refcount == destroying_refcount);

		page*   owner  = value->get_page();
		msize_t chunks = value->num_chunks;
		owner->num_objects--;
		live_objects--;

#if LI_DEBUG
		std::memset(value + 1, 0xCC, value->object_bytes());
#endif

		bool arena_tail = value->next() == owner->end() && owner == initial_page->next;
		if (arena_tail) {
			owner->next_chunk -= chunks;
			return;
		}

		value->shared      = 0;
		value->is_static   = 0;
		value->type_id     = type_gc_free;
		value->refcount    = 0;
		header*& free_list = free_lists[size_class_of(chunks)];
		value->set_next_free(free_list);
		free_list = value;
	}

	static void destroy_contents(vm* L, header* value) {
		switch (identify_value_type(value)) {
			case type_array:
				destroy(L, reinterpret_cast<array*>(value));
				break;
			case type_table:
				destroy(L, reinterpret_cast<table*>(value));
				break;
			case type_string:
				destroy(L, reinterpret_cast<string*>(value));
				break;
			case type_function:
				destroy(L, reinterpret_cast<function*>(value));
				break;
			case type_gc_proto:
				destroy(L, reinterpret_cast<function_proto*>(value));
				break;
			case type_object:
				destroy(L, reinterpret_cast<object*>(value));
				break;
			case type_class:
				destroy(L, reinterpret_cast<vclass*>(value));
				break;
			case type_weak:
				destroy(L, reinterpret_cast<weak*>(value));
				break;
			case type_typed_array:
				destroy(L, reinterpret_cast<typed_array*>(value));
				break;
#if LI_JIT
			case type_gc_jfunc:
				destroy(L, reinterpret_cast<jfunction*>(value));
				break;
#endif
			default:
				break;
		}
	}

	static void destroy_object(vm* L, header* value, bool run_finalizer) {
		invalidate_weak(L, value);
		if (run_finalizer) {
			header* previous_context = std::exchange(L->gc.finalizer_context, value);
			run_trait_finalizer(L, value);
			L->gc.finalizer_context = previous_context;
		}
		destroy_contents(L, value);
	}

	static void drain_pending(vm* L) {
		state& allocator = L->gc;
		if (allocator.destroying)
			return;

		allocator.destroying = true;
		while (!allocator.pending.empty()) {
			header* value = allocator.pending.back();
			allocator.pending.pop_back();
			destroy_object(L, value, true);
			allocator.free_storage(value);
		}
		allocator.destroying = false;
	}

	void state::close(vm* L) {
		if (shutting_down)
			L->panic("VM shutdown re-entry");
		shutting_down = true;

		L->truncate_stack(L->stack);
		L->clear_exception();
		shutdown_module_records(L);

		auto release_root = [L](auto*& slot) {
			auto* value = slot;
			slot        = nullptr;
			rc::release(L, reinterpret_cast<header*>(value));
		};
		release_root(L->modules);
		release_root(L->module_records);
		release_root(L->repl_scope);
		release_root(L->empty_string);

		header* vm_header       = static_cast<header*>(L);
		header* string_registry = reinterpret_cast<header*>(L->strset);
		header* type_registry   = reinterpret_cast<header*>(L->typeset);

		// Root-triggered finalizers above may allocate and call user code. Only
		// the final residual-cycle teardown forbids allocation.
		closing    = true;
		destroying = true;
		for_each([&](page* owner, bool) {
			owner->for_each([&](header* value) {
				if (!value->is_free() && value != vm_header && value != string_registry && value != type_registry) {
					LI_ASSERT(value->refcount != destroying_refcount);
					value->refcount = destroying_refcount;
					pending.push_back(value);
				}
				return false;
			});
			return false;
		});
		// Strong cycles never reach a last release, so user finalizers do not run
		// for this shutdown-only forced reclamation pass.
		for (header* value : pending)
			destroy_object(L, value, false);
		for (auto it = pending.rbegin(); it != pending.rend(); ++it)
			free_storage(*it);
		pending.clear();
		destroying = false;

		release_root(L->strset);
		release_root(L->typeset);
		shutdown_weak(L);
		LI_ASSERT(live_objects == 1);

		std::vector<header*>{}.swap(pending);
		auto* allocator         = alloc_fn;
		void* allocator_context = alloc_ctx;
		page* writable_head     = initial_page;

		for (page* current = writable_head->next; current != writable_head;) {
			page* next_value = current->next;
			allocator(allocator_context, current, current->num_pages, false);
			current = next_value;
		}
		allocator(allocator_context, writable_head, writable_head->num_pages, false);
		allocator(allocator_context, allocator_context, 0, false);
	}
}

namespace li::shared::detail {
	static thread_local vm* active_release_caller = nullptr;

	void run_finalizer(vm* caller, gc::header* value) {
		gc::header* previous_context = std::exchange(caller->gc.finalizer_context, value);
		run_trait_finalizer(caller, value);
		caller->gc.finalizer_context = previous_context;
	}

	void destroy_object(vm* caller, gc::header* value) {
		vm* previous_caller = std::exchange(active_release_caller, caller);
		gc::destroy_contents(shared::allocator_vm(), value);
		active_release_caller = previous_caller;
	}

	vm* release_caller(vm* fallback) { return active_release_caller ? active_release_caller : fallback; }
}

namespace li::rc {
	[[noreturn]] static void fail(vm* L, const char* message) {
		if (L)
			L->panic(message);
		util::abort("li panic: %s", message);
	}

	void retain(gc::header* value) {
		if (!value || value->is_static)
			return;
		if (value->shared) {
			shared::retain(value);
			return;
		}
		uint32_t current = value->refcount;
		if (current == 0 || current == gc::destroying_refcount)
			fail(nullptr, "attempted to resurrect a destroyed value");
		if (current == gc::maximum_refcount)
			fail(nullptr, "reference count overflow");
		value->refcount = current + 1;
		counts().retains++;
	}

	void retain(any_t value) {
		if (value.is_gc())
			retain(value.as_gc());
	}

	bool check_store(vm* L, gc::header* value) {
		if (!value || value->is_static)
			return true;
		if (!L)
			fail(nullptr, "checked store requires a VM");
		uint32_t current = value->shared ? std::atomic_ref<uint32_t>(value->refcount).load(std::memory_order_acquire) : value->refcount;
		if (current == gc::destroying_refcount) {
			L->error("cannot retain a finalizing value");
			return false;
		}
		if (current == 0)
			fail(L, "attempted to store a destroyed value");
		return true;
	}

	bool check_store(vm* L, any_t value) { return !value.is_gc() || check_store(L, value.as_gc()); }

	bool try_retain(vm* L, gc::header* value) {
		if (value && value->shared) {
			if (!shared::try_retain(value)) {
				L->error("cannot retain a finalizing value");
				return false;
			}
			return true;
		}
		if (!check_store(L, value))
			return false;
		retain(value);
		return true;
	}

	bool try_retain(vm* L, any_t value) { return !value.is_gc() || try_retain(L, value.as_gc()); }

	void retain_frame(vm* L, gc::header* value) {
		if (!value || value->is_static)
			return;
		if (!L)
			fail(nullptr, "frame retain requires a VM");
		uint32_t current = value->shared ? std::atomic_ref<uint32_t>(value->refcount).load(std::memory_order_acquire) : value->refcount;
		if (current == gc::destroying_refcount) {
			if (L->gc.finalizer_context == value)
				return;
			fail(L, "destroying value escaped its finalizer context");
		}
		retain(value);
	}

	void retain_frame(vm* L, any_t value) {
		if (value.is_gc())
			retain_frame(L, value.as_gc());
	}

	void release(vm* L, gc::header* value) {
		if (!value || value->is_static)
			return;
		if (!L)
			fail(nullptr, "dynamic value released without a VM");
		if (value->shared) {
			shared::release(shared::detail::release_caller(L), value);
			return;
		}

		uint32_t current = value->refcount;
		if (current == gc::destroying_refcount)
			return;
		if (current == 0) {
			if (L->gc.closing && value->is_free())
				return;
			fail(L, "reference count underflow");
		}

		counts().releases++;
		current--;
		value->refcount = current;
		if (current)
			return;

		value->refcount = gc::destroying_refcount;
		L->gc.pending.push_back(value);
		gc::drain_pending(L);
	}

	void release(vm* L, any_t value) {
		if (value.is_gc())
			release(L, value.as_gc());
	}

	void replace(vm* L, any& slot, any_t borrowed) {
		if (slot.value == borrowed.value)
			return;
		retain(borrowed);
		any previous = slot;
		slot         = any{borrowed};
		release(L, previous);
	}

	bool try_replace(vm* L, any& slot, any_t borrowed) {
		if (slot.value == borrowed.value)
			return true;
		if (!try_retain(L, borrowed))
			return false;
		any previous = slot;
		slot         = any{borrowed};
		release(L, previous);
		return true;
	}

	void replace_frame(vm* L, any& slot, any_t borrowed) {
		if (slot.value == borrowed.value)
			return;
		retain_frame(L, borrowed);
		any previous = slot;
		slot         = any{borrowed};
		release(L, previous);
	}

	void replace_adopt(vm* L, any& slot, any_t owned) {
		any previous = slot;
		slot         = any{owned};
		release(L, previous);
	}

	void clear(vm* L, any& slot) {
		any previous = slot;
		slot         = nil;
		release(L, previous);
	}
}
