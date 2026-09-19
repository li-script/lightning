#pragma once
#include <mutex>
#include <span>
#include <utility>
#include <vm/state.hpp>

namespace li {
	struct function_proto;
	struct object;
	struct string;

	namespace shared {
		// Shared values live in one process-wide heap. The allocator VM is storage-only:
		// user code always runs on the caller supplied to an operation.
		std::recursive_mutex& heap_mutex();
		vm*                   allocator_vm();

		bool is_shared(const gc::header* value);
		bool is_shared(any_t value);

		// All shared allocations pass through the storage owner's allocator while its
		// metadata lock is held. gc::state::shared_heap marks the header before return.
		template<typename T, typename... Tx>
		T* allocate(vm* caller, size_t extra_length = 0, Tx&&... args) {
			std::lock_guard guard(heap_mutex());
			vm*             owner  = allocator_vm();
			T*              result = owner->gc.create<T, Tx...>(owner, extra_length, std::forward<Tx>(args)...);
			if (!result) [[unlikely]]
				caller->panic("out of shared memory");
			return result;
		}

		// Copies an immutable string into the shared heap's intern registry. The
		// returned reference is owned. Shared strings are retained and returned.
		string* copy_string(vm* caller, const string* value);

		// Creates an explicitly shared root. Mutable child containers are never
		// implicitly shared; strings and immutable script metadata are copied.
		// The successful result is owned, and failure returns exception_marker.
		any_t make(vm* caller, any_t borrowed);

		// Atomic shared reference-count operations. Callers dispatch here only when
		// header.shared is set; private objects keep the non-atomic fast path.
		void retain(gc::header* value);
		bool try_retain(gc::header* value);
		void release(vm* caller, gc::header* value);

		// The shared observer registry and zero-transition use this same mutex.
		// invalidate_weak must run while it is held and before any finalizer.
		void invalidate_observers(vm* caller, gc::header* value);

		// Per-object recursive locks. Lock ownership is the current native thread;
		// caller->active_locks tracks every balanced acquisition for yield checks.
		void lock(vm* caller, gc::header* value);
		void unlock(vm* caller, gc::header* value);
		bool held_by_current_thread(const gc::header* value);

		class recursive_guard {
			vm*         caller_ = nullptr;
			gc::header* value_  = nullptr;

		  public:
			recursive_guard(vm* caller, gc::header* value) : caller_(caller), value_(value) { lock(caller_, value_); }
			template<typename T>
			recursive_guard(vm* caller, T* value) : recursive_guard(caller, static_cast<gc::header*>(value)) {}
			~recursive_guard() {
				if (value_)
					unlock(caller_, value_);
			}

			recursive_guard(const recursive_guard&)            = delete;
			recursive_guard& operator=(const recursive_guard&) = delete;
			recursive_guard(recursive_guard&& other) noexcept : caller_(std::exchange(other.caller_, nullptr)), value_(std::exchange(other.value_, nullptr)) {}
			recursive_guard& operator=(recursive_guard&&) = delete;
		};

		struct prepared_value {
			any  value = nil;
			bool owns  = false;
			bool ok    = false;
		};

		// Validation/copy happens before a shared destination is locked or mutated.
		prepared_value prepare_store(vm* caller, gc::header* destination, any_t borrowed);
		void           finish_store(vm* caller, prepared_value& value);

		// Native numeric primitive. The target lock pins any movable backing address;
		// validation completes before mutation and the returned number is immediate.
		any_t atomic_add(vm* caller, gc::header* target, any_t key, number delta);

		enum class numeric_operation : uint8_t {
			set,
			add,
			sub,
			mul,
			div,
			mod,
		};
		struct numeric_update {
			gc::header*       target    = nullptr;
			any               key       = nil;
			numeric_operation operation = numeric_operation::set;
			number            operand   = 0;
		};

		// Atomic strict-field primitives. Private instances use their ordinary typed
		// storage path; shared instances use a one-element atomic_update batch.
		bool  is_atomic_field(const object* target, const string* key);
		any_t atomic_field_store(vm* caller, any_t target, any_t key, any_t value);
		any_t atomic_field_update(vm* caller, any_t target, any_t key, numeric_operation operation, number operand);

		// Executes an ordered batch atomically. Every distinct target is locked in
		// address order, then every field and result is validated before any write.
		// results must have at least updates.size() entries. When the caller already
		// holds a target lock, false leaves error allocation to the caller after it
		// releases that outer lock; direct calls receive caller->last_ex normally.
		bool atomic_update(vm* caller, std::span<const numeric_update> updates, std::span<number> results);

		namespace detail {
			// Implemented beside the ordinary GC destroy dispatch. User finalizers
			// run on caller without the heap mutex; outgoing-reference and registry
			// cleanup runs separately on the storage owner under that mutex.
			void run_finalizer(vm* caller, gc::header* value);
			void destroy_object(vm* caller, gc::header* value);
			vm*  release_caller(vm* fallback);
		}
	}
}
