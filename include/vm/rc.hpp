#pragma once
#include <cstdint>
#include <vm/gc.hpp>

namespace li::rc {
	struct statistics {
		uint64_t retains  = 0;
		uint64_t releases = 0;
	};

	LI_INLINE inline statistics& counts() {
		static thread_local statistics value{};
		return value;
	}

	void retain(gc::header* value);
	void retain(any_t value);
	bool check_store(vm* L, gc::header* value);
	bool check_store(vm* L, any_t value);
	bool try_retain(vm* L, gc::header* value);
	bool try_retain(vm* L, any_t value);
	void retain_frame(vm* L, gc::header* value);
	void retain_frame(vm* L, any_t value);
	void release(vm* L, gc::header* value);
	void release(vm* L, any_t value);

	void replace(vm* L, any& slot, any_t borrowed);
	bool try_replace(vm* L, any& slot, any_t borrowed);
	void replace_frame(vm* L, any& slot, any_t borrowed);
	void replace_adopt(vm* L, any& slot, any_t owned);
	void clear(vm* L, any& slot);

	template<typename T>
	LI_INLINE void replace(vm* L, T*& slot, T* borrowed) {
		if (slot == borrowed)
			return;
		retain(static_cast<gc::header*>(borrowed));
		T* previous = slot;
		slot        = borrowed;
		release(L, static_cast<gc::header*>(previous));
	}
}
