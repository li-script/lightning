#pragma once
#include <algorithm>
#include <utility>

namespace li::util {
	template<typename T, typename T2>
	static void link_before(T* entry, T2* value) {
		auto* prev  = std::exchange(entry->prev, value);
		prev->next  = value;
		value->prev = prev;
		value->next = entry;
	}
	template<typename T, typename T2>
	static void link_after(T* entry, T2* value) {
		auto* next  = std::exchange(entry->next, value);
		next->prev  = value;
		value->prev = entry;
		value->next = next;
	}
	template<typename T>
	static void unlink(T* entry) {
		auto* prev  = std::exchange(entry->prev, entry);
		auto* next  = std::exchange(entry->next, entry);
		prev->next  = next;
		next->prev  = prev;
	}
};