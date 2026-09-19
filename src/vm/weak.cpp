#include <algorithm>
#include <unordered_map>
#include <vector>
#include <vm/rc.hpp>
#include <vm/shared.hpp>
#include <vm/state.hpp>
#include <vm/weak.hpp>

namespace li {
	namespace {
		struct weak_registry {
			std::unordered_map<gc::header*, std::vector<weak*>> targets;
		};

		static weak_registry* registry(vm* L) { return static_cast<weak_registry*>(L->gc.weak_registry); }

		static weak_registry* ensure_registry(vm* L) {
			auto* result = registry(L);
			if (!result) {
				result              = new weak_registry();
				L->gc.weak_registry = result;
			}
			return result;
		}

		static void release_empty_registry(vm* L, weak_registry* value) {
			if (value->targets.empty()) {
				delete value;
				L->gc.weak_registry = nullptr;
			}
		}

		weak_registry& shared_registry() {
			static weak_registry value;
			return value;
		}

		void unregister_observer(weak_registry& records, gc::header* target, weak* value) {
			auto it = records.targets.find(target);
			LI_ASSERT(it != records.targets.end());
			auto& observers = it->second;
			auto  observer  = std::find(observers.begin(), observers.end(), value);
			LI_ASSERT(observer != observers.end());
			*observer = observers.back();
			observers.pop_back();
			if (observers.empty())
				records.targets.erase(it);
		}
	}

	weak* weak::create(vm* L, gc::header* target) {
		LI_ASSERT(target != nullptr);
		weak* result            = target->shared ? shared::allocate<weak>(L) : L->alloc<weak>();
		result->observes_shared = target->shared;
		if (result->observes_shared) {
			std::lock_guard guard(shared::heap_mutex());
			if (!shared::try_retain(target))
				L->panic("cannot create a weak reference to a destroyed shared value");
			shared::release(L, target);
			result->target = target;
			shared_registry().targets[target].push_back(result);
		} else {
			result->target = target;
			ensure_registry(L)->targets[target].push_back(result);
		}
		return result;
	}

	any_t weak::lock() const {
		if (observes_shared) {
			std::lock_guard guard(shared::heap_mutex());
			gc::header*     value = target;
			if (!value || !shared::try_retain(value))
				return nil;
			return any(value);
		}

		gc::header* value = target;
		if (!value || (!value->is_static && value->refcount == gc::destroying_refcount))
			return nil;
		rc::retain(value);
		return any(value);
	}

	bool weak::expired() const {
		if (!observes_shared)
			return target == nullptr;
		std::lock_guard guard(shared::heap_mutex());
		return target == nullptr;
	}

	void gc::invalidate_weak(vm* L, gc::header* target) {
		if (target->shared) {
			std::lock_guard guard(shared::heap_mutex());
			auto&           records = shared_registry();
			auto            it      = records.targets.find(target);
			if (it == records.targets.end())
				return;
			for (weak* observer : it->second)
				observer->target = nullptr;
			records.targets.erase(it);
			return;
		}

		auto* records = registry(L);
		if (!records)
			return;
		auto it = records->targets.find(target);
		if (it == records->targets.end())
			return;
		for (weak* observer : it->second)
			observer->target = nullptr;
		records->targets.erase(it);
		release_empty_registry(L, records);
	}

	void gc::destroy(vm* L, weak* value) {
		if (value->observes_shared) {
			std::lock_guard guard(shared::heap_mutex());
			gc::header*     target = value->target;
			if (target)
				unregister_observer(shared_registry(), target, value);
			value->target          = nullptr;
			value->observes_shared = false;
			return;
		}

		gc::header* target = value->target;
		if (!target)
			return;
		auto* records = registry(L);
		LI_ASSERT(records != nullptr);
		unregister_observer(*records, target, value);
		value->target = nullptr;
		release_empty_registry(L, records);
	}

	void gc::shutdown_weak(vm* L) {
		auto* records = registry(L);
		if (!records)
			return;
		for (auto& entry : records->targets) {
			for (weak* observer : entry.second)
				observer->target = nullptr;
		}
		delete records;
		L->gc.weak_registry = nullptr;
	}
}

namespace li::shared {
	void invalidate_observers(vm* caller, gc::header* value) { gc::invalidate_weak(caller, value); }
}
