#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <lang/parser.hpp>
#include <lib/std.hpp>
#include <string_view>
#include <thread>
#include <vector>
#include <vm/array.hpp>
#include <vm/function.hpp>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/shared.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <vm/tier.hpp>
#include <vm/weak.hpp>

namespace {
	using namespace li;

	[[noreturn]] void fail(const char* message) {
		std::fprintf(stderr, "shared runtime regression: %s\n", message);
		std::abort();
	}

	void require(bool condition, const char* message) {
		if (!condition)
			fail(message);
	}

	string* key(vm* L, std::string_view value) { return string::create(L, value); }

	function* shared_export(vm* L, std::string_view name) {
		string* module_key = key(L, "shared");
		any     module     = L->modules->get(L, any(module_key));
		rc::release(L, module_key);
		if (!module.is_tbl())
			return nullptr;

		string* export_key = key(L, name);
		any     value      = module.as_tbl()->get(L, any(export_key));
		rc::release(L, export_key);
		return value.is_fn() ? value.as_fn() : nullptr;
	}

	table* shared_counter(vm* L, number initial, std::string_view field = "counter") {
		table*  source    = table::create(L);
		string* field_key = key(L, field);
		require(source->set(L, any(field_key), any(initial)), "failed to initialize private counter source");
		rc::release(L, field_key);

		any result = shared::make(L, any(source));
		rc::release(L, source);
		require(!result.is_exc() && result.is_tbl(), "shared.create core did not copy a table");
		require(shared::is_shared(result), "shared table was not marked shared");
		return result.as_tbl();
	}

	array* shared_queue(vm* L) {
		array* source = array::create(L);
		any    result = shared::make(L, any(source));
		rc::release(L, source);
		require(!result.is_exc() && result.is_arr(), "shared.create core did not copy an array");
		return result.as_arr();
	}

	void two_vm_queue_and_recursive_lock() {
		vm* owner = vm::create();
		require(owner != nullptr, "failed to create queue owner VM");
		array* queue = shared_queue(owner);

		shared::retain(queue);
		shared::retain(queue);
		constexpr unsigned    item_count = 20000;
		std::atomic<bool>     producer_done{false};
		std::atomic<bool>     failed{false};
		std::atomic<uint64_t> consumed_sum{0};

		std::thread producer([&] {
			vm* L = vm::create();
			if (!L) {
				failed.store(true, std::memory_order_relaxed);
				producer_done.store(true, std::memory_order_release);
				return;
			}

			shared::lock(L, queue);
			if (L->active_locks != 1)
				failed.store(true, std::memory_order_relaxed);
			shared::lock(L, queue);
			if (L->active_locks != 2)
				failed.store(true, std::memory_order_relaxed);
			shared::unlock(L, queue);
			if (L->active_locks != 1)
				failed.store(true, std::memory_order_relaxed);
			shared::unlock(L, queue);
			if (L->active_locks != 0)
				failed.store(true, std::memory_order_relaxed);

			for (unsigned value = 1; value <= item_count; ++value) {
				shared::recursive_guard guard(L, queue);
				if (!queue->push(L, any(number(value)))) {
					failed.store(true, std::memory_order_relaxed);
					break;
				}
			}
			producer_done.store(true, std::memory_order_release);
			rc::release(L, queue);
			L->close();
		});

		std::thread consumer([&] {
			vm* L = vm::create();
			if (!L) {
				failed.store(true, std::memory_order_relaxed);
				return;
			}

			unsigned consumed = 0;
			uint64_t sum      = 0;
			while (consumed != item_count) {
				any value = nil;
				{
					shared::recursive_guard guard(L, queue);
					if (queue->size() != 0)
						value = queue->pop();
				}
				if (value.is_num()) {
					++consumed;
					sum += static_cast<uint64_t>(value.as_num());
					continue;
				}
				if (producer_done.load(std::memory_order_acquire) && failed.load(std::memory_order_relaxed))
					break;
				std::this_thread::yield();
			}
			consumed_sum.store(sum, std::memory_order_relaxed);
			rc::release(L, queue);
			L->close();
		});

		producer.join();
		consumer.join();
		require(!failed.load(std::memory_order_relaxed), "two-VM queue or recursive lock accounting failed");
		const uint64_t expected = uint64_t(item_count) * uint64_t(item_count + 1) / 2;
		require(consumed_sum.load(std::memory_order_relaxed) == expected, "two-VM shared queue lost or duplicated a value");
		require(queue->size() == 0, "two-VM shared queue was not drained");

		rc::release(owner, queue);
		owner->close();
	}

	void eight_thread_atomic_counter() {
		vm* owner = vm::create();
		require(owner != nullptr, "failed to create counter owner VM");
		table* counter = shared_counter(owner, 0);

		constexpr unsigned thread_count = 8;
		constexpr unsigned iterations   = 25000;
		for (unsigned i = 0; i != thread_count; ++i)
			shared::retain(counter);

		std::atomic<bool>        failed{false};
		std::vector<std::thread> workers;
		workers.reserve(thread_count);
		for (unsigned thread_index = 0; thread_index != thread_count; ++thread_index) {
			workers.emplace_back([&, thread_index] {
				(void) thread_index;
				vm* L = vm::create();
				if (!L) {
					failed.store(true, std::memory_order_relaxed);
					return;
				}
				string* counter_key = key(L, "counter");
				for (unsigned iteration = 0; iteration != iterations; ++iteration) {
					any result = shared::atomic_add(L, counter, any(counter_key), 1);
					if (!result.is_num()) {
						failed.store(true, std::memory_order_relaxed);
						break;
					}
				}
				rc::release(L, counter_key);
				rc::release(L, counter);
				L->close();
			});
		}
		for (std::thread& worker : workers)
			worker.join();

		require(!failed.load(std::memory_order_relaxed), "shared atomic_add failed in a worker VM");
		string* counter_key = key(owner, "counter");
		any     final_value = counter->get(owner, any(counter_key));
		rc::release(owner, counter_key);
		require(final_value.is_num() && final_value.as_num() == number(thread_count * iterations), "eight-thread atomic counter was not exact");

		rc::release(owner, counter);
		owner->close();
	}

	void eight_thread_strict_atomic_field() {
		static constexpr std::string_view source = R"li(
[[strict]]
class Counter {
   atomic count: i32 = 0i32
}
fn increment(counter: Counter) -> i32 {
   counter.count += 1i32
}
[Counter::new(), increment]
)li";

		vm* owner = vm::create();
		require(owner != nullptr, "failed to create strict atomic-field owner VM");
		lib::register_std(owner);
		any script = load_script(owner, source, "strict-atomic-field");
		require(!script.is_exc() && script.is_fn(), "failed to compile strict atomic-field fixture");
		any bundle_value = owner->call(0, script);
		rc::release(owner, script);
		require(!bundle_value.is_exc() && bundle_value.is_arr(), "strict atomic-field fixture returned malformed values");
		array* bundle            = bundle_value.as_arr();
		any    private_counter   = bundle->get(owner, 0);
		any    private_increment = bundle->get(owner, 1);
		require(private_counter.is_obj() && private_increment.is_fn(), "strict atomic-field fixture did not return an object and function");

		any counter_value   = shared::make(owner, private_counter);
		any increment_value = shared::make(owner, private_increment);
		require(counter_value.is_obj() && shared::is_shared(counter_value), "strict atomic counter was not shared");
		require(increment_value.is_fn() && shared::is_shared(increment_value), "strict atomic increment function was not shared");
		object*   counter   = counter_value.as_obj();
		function* increment = increment_value.as_fn();
#if LI_JIT
		auto jit_error = tier::compile_required(owner, increment);
		require(!jit_error.has_value(), "strict atomic increment function did not compile");
#endif

		constexpr unsigned thread_count = 8;
		constexpr unsigned iterations   = 10000;
		for (unsigned index = 0; index != thread_count; ++index) {
			shared::retain(counter);
			shared::retain(increment);
		}

		std::atomic<bool>        failed{false};
		std::vector<std::thread> workers;
		workers.reserve(thread_count);
		for (unsigned thread_index = 0; thread_index != thread_count; ++thread_index) {
			workers.emplace_back([&, thread_index] {
				(void) thread_index;
				vm* L = vm::create();
				if (!L) {
					failed.store(true, std::memory_order_relaxed);
					return;
				}
				for (unsigned iteration = 0; iteration != iterations; ++iteration) {
					L->push_stack(any(counter));
					any result = L->call(1, any(increment));
					if (!result.is_num()) {
						failed.store(true, std::memory_order_relaxed);
						break;
					}
				}
				rc::release(L, increment);
				rc::release(L, counter);
				L->close();
			});
		}
		for (std::thread& worker : workers)
			worker.join();

		require(!failed.load(std::memory_order_relaxed), "compiled strict atomic field update failed in a worker VM");
		string* count_key = key(owner, "count");
		any     final     = counter->get(count_key);
		rc::release(owner, count_key);
		require(final.is_num() && final.as_num() == 80000, "compiled strict atomic field counter was not exactly 80000");

		rc::release(owner, increment);
		rc::release(owner, counter);
		rc::release(owner, bundle_value);
		owner->close();
	}

	void numeric_transaction_is_all_or_nothing() {
		vm* L = vm::create();
		require(L != nullptr, "failed to create numeric transaction VM");
		table*  source    = table::create(L);
		string* left_key  = key(L, "left");
		string* right_key = key(L, "right");
		require(source->set(L, any(left_key), any(number(10))), "failed to initialize left transaction field");
		require(source->set(L, any(right_key), any(number(4))), "failed to initialize right transaction field");
		any copied = shared::make(L, any(source));
		rc::release(L, source);
		require(copied.is_tbl(), "failed to create transaction target");
		table* target = copied.as_tbl();

		std::array<shared::numeric_update, 2> updates{{
			 {.target = target, .key = any(left_key), .operation = shared::numeric_operation::add, .operand = 5},
			 {.target = target, .key = any(right_key), .operation = shared::numeric_operation::mul, .operand = 3},
		}};
		std::array<number, 2>                 results{};
		require(shared::atomic_update(L, updates, results), "valid numeric transaction failed");
		require(results[0] == 15 && results[1] == 12, "numeric transaction returned incorrect results");
		require(target->get(L, any(left_key)).as_num() == 15 && target->get(L, any(right_key)).as_num() == 12, "numeric transaction did not publish every field");

		string* missing_key = key(L, "missing");
		updates[0]          = {.target = target, .key = any(left_key), .operation = shared::numeric_operation::add, .operand = 100};
		updates[1]          = {.target = target, .key = any(missing_key), .operation = shared::numeric_operation::add, .operand = 1};
		require(!shared::atomic_update(L, updates, results), "invalid numeric transaction unexpectedly succeeded");
		require(L->last_ex.is_str(), "invalid numeric transaction did not report a catchable error");
		require(target->get(L, any(left_key)).as_num() == 15, "failed numeric transaction published a partial write");
		L->clear_exception();

		rc::release(L, missing_key);
		rc::release(L, right_key);
		rc::release(L, left_key);
		rc::release(L, target);
		L->close();
	}

	void source_vm_can_close_first() {
		vm* source = vm::create();
		require(source != nullptr, "failed to create source VM");
		table*  value       = table::create(source);
		string* message_key = key(source, "message");
		string* message     = string::create(source, "survives-source-close");
		require(value->set(source, any(message_key), any(message)), "failed to initialize source-close value");
		rc::release(source, message_key);
		rc::release(source, message);

		any copied = shared::make(source, any(value));
		rc::release(source, value);
		require(copied.is_tbl(), "failed to make source-close value shared");
		table* shared_value = copied.as_tbl();
		shared::retain(shared_value);

		std::atomic<bool> consumer_ready{false};
		std::atomic<bool> source_closed{false};
		std::atomic<bool> observed{false};
		std::thread       consumer([&] {
			vm* L = vm::create();
			if (!L) {
				consumer_ready.store(true, std::memory_order_release);
				return;
			}
			consumer_ready.store(true, std::memory_order_release);
			while (!source_closed.load(std::memory_order_acquire))
				std::this_thread::yield();

			string* lookup_key = key(L, "message");
			any     stored     = shared_value->get(L, any(lookup_key));
			observed.store(stored.is_str() && stored.as_str()->view() == "survives-source-close", std::memory_order_relaxed);
			rc::release(L, lookup_key);
			rc::release(L, shared_value);
			L->close();
		});

		while (!consumer_ready.load(std::memory_order_acquire))
			std::this_thread::yield();
		rc::release(source, shared_value);
		source->close();
		source_closed.store(true, std::memory_order_release);
		consumer.join();
		require(observed.load(std::memory_order_relaxed), "shared value depended on its source VM lifetime");
	}

	void native_lock_errors_are_catchable() {
		vm* owner = vm::create();
		require(owner != nullptr, "failed to create lock owner VM");
		table* target = shared_counter(owner, 1);
		shared::lock(owner, target);
		shared::retain(target);

		std::atomic<bool> foreign_rejected{false};
		std::thread       foreign([&] {
			vm* L = vm::create();
			if (!L)
				return;
			lib::register_std(L);
			function* unlock = shared_export(L, "unlock");
			if (unlock) {
				L->push_stack(any(target));
				any result = L->call(1, any(unlock));
				foreign_rejected.store(result.is_exc() && L->last_ex.is_str(), std::memory_order_relaxed);
				L->clear_exception();
			}
			rc::release(L, target);
			L->close();
		});
		foreign.join();
		require(foreign_rejected.load(std::memory_order_relaxed), "foreign shared.unlock was not a catchable language error");
		require(shared::held_by_current_thread(target), "foreign unlock disturbed the owning thread's lock");
		shared::unlock(owner, target);

		lib::register_std(owner);
		function* lock = shared_export(owner, "lock");
		require(lock != nullptr, "shared.lock native export was not registered");
		table* private_value = table::create(owner);
		owner->push_stack(any(private_value));
		any private_result = owner->call(1, any(lock));
		require(private_result.is_exc() && owner->last_ex.is_str(), "private shared.lock misuse was not catchable");
		owner->clear_exception();
		rc::release(owner, private_value);

		rc::release(owner, target);
		owner->close();
	}

	void rejected_private_edge_is_non_mutating() {
		vm* L = vm::create();
		require(L != nullptr, "failed to create private-edge VM");
		table*         destination    = shared_counter(L, 7, "stable");
		table*         private_child  = table::create(L);
		string*        child_key      = key(L, "child");
		const uint64_t version_before = destination->mutation_version;

		require(!destination->set(L, any(child_key), any(private_child)), "private child was stored in a shared table");
		require(L->last_ex.is_str(), "private shared-store rejection was not language-catchable");
		require(destination->mutation_version == version_before, "failed shared store changed the mutation version");
		require(destination->get(L, any(child_key)) == nil, "failed shared store mutated its destination");
		L->clear_exception();

		rc::release(L, child_key);
		rc::release(L, private_child);
		rc::release(L, destination);
		L->close();
	}

	void shared_weak_expires_on_last_release() {
		vm* L = vm::create();
		require(L != nullptr, "failed to create shared-weak VM");
		table* target   = shared_counter(L, 1);
		weak*  observer = weak::create(L, target);

		require(!observer->expired(), "shared weak observer started expired");

		any locked = observer->lock();
		require(locked.is_tbl() && locked.as_tbl() == target, "shared weak lock did not acquire a strong reference");
		rc::release(L, target);
		require(!observer->expired(), "shared weak target expired before the locked reference was released");
		rc::release(L, locked);
		require(observer->expired(), "shared weak observer survived the target's last release");
		require(observer->lock() == nil, "expired shared weak observer returned freed storage");

		rc::release(L, observer);
		L->close();
	}

	void shared_factory_creates_caller_private_values() {
		static constexpr std::string_view source = R"li(
class FactoryBox {
   value: number = 7
}
fn make_values() {
   let counter = 40
   const callback = || {
      counter += 1
      counter
   }
   {instance: FactoryBox{}, callback: callback}
}
make_values
)li";

		vm* producer = vm::create();
		require(producer != nullptr, "failed to create shared-factory producer VM");
		any script = load_script(producer, source, "shared-factory-producer");
		require(!script.is_exc() && script.is_fn(), "failed to compile shared-factory producer");
		any private_factory = producer->call(0, script);
		require(!private_factory.is_exc() && private_factory.is_fn(), "failed to construct private factory");
		any shared_factory = shared::make(producer, private_factory);
		require(!shared_factory.is_exc() && shared_factory.is_fn(), "failed to explicitly share factory");
		require(shared::is_shared(shared_factory), "explicitly shared factory remained private");
		require(shared_factory.as_fn()->proto && shared_factory.as_fn()->proto->shared, "shared factory did not own shared code metadata");
		rc::release(producer, private_factory);
		rc::release(producer, script);
		producer->close();

		vm* consumer = vm::create();
		require(consumer != nullptr, "failed to create shared-factory consumer VM");
		any bundle = consumer->call(0, shared_factory);
		require(!bundle.is_exc() && bundle.is_tbl(), "shared factory failed after producer VM closed");

		string* instance_key = key(consumer, "instance");
		string* callback_key = key(consumer, "callback");
		string* value_key    = key(consumer, "value");
		any     instance     = bundle.as_tbl()->get(consumer, any(instance_key));
		any     callback     = bundle.as_tbl()->get(consumer, any(callback_key));
		require(instance.is_obj() && callback.is_fn(), "shared factory returned malformed values");
		require(!shared::is_shared(instance), "shared class metadata implicitly produced a shared instance");
		require(instance.as_obj()->cl->shared, "factory instance did not retain shared class metadata");
		require(!shared::is_shared(callback), "shared prototype implicitly produced a shared closure");
		require(callback.as_fn()->proto && callback.as_fn()->proto->shared, "factory closure did not retain shared prototype metadata");

		require(instance.as_obj()->get(value_key).is_num() && instance.as_obj()->get(value_key).as_num() == 7, "caller-private instance was not usable");
		require(instance.as_obj()->set(consumer, value_key, any(number(9))), "caller-private instance mutation failed");
		require(
			 instance.as_obj()->get(value_key).is_num() && instance.as_obj()->get(value_key).as_num() == 9, "caller-private instance mutation was not observable");
		any first_callback = consumer->call(0, callback);
		any next_callback  = consumer->call(0, callback);
		require(first_callback.is_num() && first_callback.as_num() == 41 && next_callback.is_num() && next_callback.as_num() == 42,
			 "caller-private closure did not preserve mutable captured state");
		rc::release(consumer, first_callback);
		rc::release(consumer, next_callback);

		table*  transfer     = shared_counter(consumer, 0);
		string* object_slot  = key(consumer, "object");
		string* closure_slot = key(consumer, "closure");
		require(!transfer->set(consumer, any(object_slot), instance), "private instance crossed a shared transfer boundary");
		require(consumer->last_ex.is_str(), "private instance transfer rejection was not catchable");
		consumer->clear_exception();
		require(!transfer->set(consumer, any(closure_slot), callback), "private closure crossed a shared transfer boundary");
		require(consumer->last_ex.is_str(), "private closure transfer rejection was not catchable");
		consumer->clear_exception();

		any shared_instance = shared::make(consumer, instance);
		any shared_callback = shared::make(consumer, callback);
		require(!shared_instance.is_exc() && shared_instance.is_obj() && shared::is_shared(shared_instance),
			 "explicit instance sharing did not create shared storage");
		require(
			 !shared_callback.is_exc() && shared_callback.is_fn() && shared::is_shared(shared_callback), "explicit closure sharing did not create shared storage");
		shared::lock(consumer, shared_instance.as_gc());
		require(shared::held_by_current_thread(shared_instance.as_gc()), "explicitly shared instance did not have shared locking");
		shared::unlock(consumer, shared_instance.as_gc());
		shared::lock(consumer, shared_callback.as_gc());
		require(shared::held_by_current_thread(shared_callback.as_gc()), "explicitly shared closure did not have shared locking");
		shared::unlock(consumer, shared_callback.as_gc());
		require(shared_instance.as_obj()->get(value_key).is_num() && shared_instance.as_obj()->get(value_key).as_num() == 9,
			 "explicitly shared instance did not preserve its value");
		any shared_callback_result = consumer->call(0, shared_callback);
		require(shared_callback_result.is_num() && shared_callback_result.as_num() == 43, "explicitly shared closure did not preserve its captured state");
		rc::release(consumer, shared_callback_result);
		require(transfer->set(consumer, any(object_slot), shared_instance), "explicitly shared instance was rejected by shared transfer");
		require(transfer->set(consumer, any(closure_slot), shared_callback), "explicitly shared closure was rejected by shared transfer");
		require(shared::is_shared(transfer), "existing shared table allocation became private");

		rc::release(consumer, shared_instance);
		rc::release(consumer, shared_callback);
		rc::release(consumer, object_slot);
		rc::release(consumer, closure_slot);
		rc::release(consumer, transfer);
		rc::release(consumer, value_key);
		rc::release(consumer, callback_key);
		rc::release(consumer, instance_key);
		rc::release(consumer, bundle);
		rc::release(consumer, shared_factory);
		consumer->close();
	}
}

int main() {
	two_vm_queue_and_recursive_lock();
	eight_thread_atomic_counter();
	eight_thread_strict_atomic_field();
	source_vm_can_close_first();
	numeric_transaction_is_all_or_nothing();
	native_lock_errors_are_catchable();
	rejected_private_edge_is_non_mutating();
	shared_weak_expires_on_last_release();
	shared_factory_creates_caller_private_values();
	return 0;
}
