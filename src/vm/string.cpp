#include <atomic>
#include <cstring>

#include <util/common.hpp>
#include <vm/function.hpp>
#include <vm/rc.hpp>
#include <vm/shared.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>

namespace li {
	// Sparse string hasher.
	//
	static uint32_t sparse_hash(std::string_view v) {
		const char* str = v.data();
		uint32_t    len = (uint32_t) v.size();

		if (len == 0)
			return 0;

		auto load_u32 = [](const char* src) {
			uint32_t value;
			std::memcpy(&value, src, sizeof(value));
			return value;
		};

#if LI_HAS_CRC
		uint32_t crc = len;
		if (len >= 4) {
			crc = _mm_crc32_u32(crc, load_u32(str));
			crc = _mm_crc32_u32(crc, load_u32(str + len - 4));
			crc = _mm_crc32_u32(crc, load_u32(str + (len >> 1) - 2));
			crc = _mm_crc32_u32(crc, load_u32(str + (len >> 2) - 1));
		} else {
			crc = _mm_crc32_u8(crc, *(const uint8_t*) str);
			crc = _mm_crc32_u8(crc, *(const uint8_t*) (str + len - 1));
			crc = _mm_crc32_u8(crc, *(const uint8_t*) (str + (len >> 1)));
		}
		return crc;
#else
		uint32_t a, b;
		uint32_t h = len ^ 0xd3cccc57;
		if (len >= 4) {
			a = load_u32(str);
			h ^= load_u32(str + len - 4);
			b = load_u32(str + (len >> 1) - 2);
			h ^= b;
			h -= std::rotl(b, 14);
			b += load_u32(str + (len >> 2) - 1);
		} else {
			a = *(const uint8_t*) str;
			h ^= *(const uint8_t*) (str + len - 1);
			b = *(const uint8_t*) (str + (len >> 1));
			h ^= b;
			h -= std::rotl(b, 14);
		}
		a ^= h;
		a -= std::rotl(h, 11);
		b ^= a;
		b -= std::rotl(a, 25);
		h ^= b;
		h -= std::rotl(b, 16);
		return h;
#endif
	}

	bool LI_CC string_value_equals(const string* lhs, const string* rhs) noexcept {
		if (lhs == rhs)
			return true;
		if (!lhs || !rhs || lhs->length != rhs->length || lhs->hash != rhs->hash)
			return false;
		return std::memcmp(lhs->data, rhs->data, lhs->length) == 0;
	}

	size_t LI_CC string_value_hash(const string* value) noexcept { return value ? size_t(value->hash) : 0; }

	struct string_set : gc::leaf<string_set> {
		static constexpr size_t min_size        = 512;
		static constexpr size_t overflow_factor = 3;

		string* entries[];

		// Expose the same interface as table.
		//
		size_t             size() { return std::bit_floor((this->object_bytes() / sizeof(string*)) - overflow_factor); }
		size_t             mask() { return size() - 1; }
		string**           begin() { return &entries[0]; }
		string**           end() { return &entries[size() + overflow_factor]; }
		std::span<string*> find(size_t hash) {
			auto it = begin() + (hash & mask());
			return {it, end()};  // allow full load.
		}

		static bool retired_entry(string* entry) {
			if (!entry)
				return true;
			uint32_t count = entry->shared ? std::atomic_ref<uint32_t>(entry->refcount).load(std::memory_order_acquire) : entry->refcount;
			return count == 0 || count == gc::destroying_refcount;
		}

		static bool retain_entry(string*& entry) {
			if (entry->shared) {
				if (!shared::try_retain(entry)) {
					// The shared zero transition won the race. Retire the weak
					// registry slot so a replacement may be interned immediately.
					entry = nullptr;
					return false;
				}
			} else {
				if (entry->refcount == gc::destroying_refcount) {
					entry = nullptr;
					return false;
				}
				rc::retain(entry);
			}
			return true;
		}

		// Simpler implementation of the same algorithm as table with no fixed holder.
		//
		[[nodiscard]] string_set* push(vm* L, string* s) {
			string_set* ss = this;
			while (true) {
				for (auto& entry : ss->find(s->hash)) {
					if (retired_entry(entry)) {
						entry = s;
						return ss;
					}
				}
				string_set* old_set = ss;
				ss                  = ss->nextsize(L);
				LI_ASSERT(L->strset == old_set);
				L->strset = ss;
				rc::release(L, old_set);
			}
			return ss;
		}
		[[nodiscard]] string_set* nextsize(vm* L) {
			size_t old_count = size();
			size_t new_count = old_count << 1;

			string_set* new_set = L->alloc<string_set>(sizeof(string*) * (new_count + overflow_factor));
			std::fill_n(new_set->entries, new_count + overflow_factor, nullptr);

			for (size_t i = 0; i != (old_count + overflow_factor); i++) {
				if (string* s = entries[i]; !retired_entry(s)) {
					new_set = new_set->push(L, s);
				}
			}

			return new_set;
		}
		static string* push_if(vm* L, string* s) {
			LI_ASSERT(s->length != 0);
			s->hash = sparse_hash(s->view());

			// Return if already exists.
			//
			for (auto& entry : L->strset->find(s->hash)) {
				if (entry && entry->view() == s->view() && retain_entry(entry)) {
					rc::release(L, s);
					return entry;
				}
			}

			// Push and return.
			//
			L->strset = L->strset->push(L, s);
			return s;
		}
		static string* push(vm* L, std::string_view key) {
			if (key.empty()) [[unlikely]] {
				return string::create(L);
			}

			uint32_t hash = sparse_hash(key);

			// Return if already exists.
			//
			for (auto& entry : L->strset->find(hash)) {
				if (entry && entry->view() == key && retain_entry(entry))
					return entry;
			}

			// Create and push otherwise.
			//
			string* str = L->alloc<string>(key.size() + 1);
			memcpy(str->data, key.data(), key.size());
			str->data[key.size()] = 0;
			str->length           = (uint32_t) key.size();
			str->hash             = hash;
			L->strset             = L->strset->push(L, str);
			return str;
		}
	};

	// Internal string-set implementation.
	//
	void strset_init(vm* L) {
		L->strset = L->alloc<string_set>(sizeof(string*) * string_set::min_size);
		std::fill_n(L->strset->entries, string_set::min_size, nullptr);

		string* str     = L->alloc<string>(1);
		str->data[0]    = 0;
		str->length     = 0;
		str->hash       = 0;
		L->empty_string = str;
	}

	void strset_reset_shared(vm* owner) {
		LI_ASSERT(owner && owner->gc.shared_heap);
		string_set* previous = owner->strset;
		LI_ASSERT(previous && !previous->shared);
		owner->strset = owner->alloc<string_set>(sizeof(string*) * string_set::min_size);
		std::fill_n(owner->strset->entries, string_set::min_size, nullptr);
		rc::release(owner, previous);
	}

	void strset_remove(vm* L, string* value) {
		if (!L->strset)
			return;

		for (auto& entry : L->strset->find(value->hash)) {
			if (entry == value) {
				entry = nullptr;
				return;
			}
		}
	}

	// String creation.
	//
	string* string::create(vm* L, std::string_view from) { return string_set::push(L, from); }
	string* string::format(vm* L, const char* fmt, ...) {
		va_list a1;
		va_start(a1, fmt);

		// First try formatting on stack:
		//
		va_list a2;
		va_copy(a2, a1);
		char buffer[64];
		int  ns = vsnprintf(buffer, std::size(buffer), fmt, a2);
		va_end(a2);

		// If empty, handle.
		//
		if (ns <= 0) {
			va_end(a1);
			return string::create(L);
		}
		uint32_t n = uint32_t(ns);

		// If it did fit, forward to string::create with a view:
		//
		if (n < std::size(buffer)) {
			va_end(a1);
			return string::create(L, {buffer, (size_t) n});
		}

		// Otherwise, allocate a string and format into it.
		//
		string* str = L->alloc<string>(n + 1);
		str->length = n;
		vsnprintf(str->data, n + 1, fmt, a1);
		va_end(a1);
		return string_set::push_if(L, str);
	}
	string* string::concat(vm* L, string* a, string* b) {
		// Preserve ownership when either operand is any intern pool's empty string.
		//
		if (a->length == 0) [[unlikely]] {
			rc::retain(b);
			return b;
		}
		if (b->length == 0) [[unlikely]] {
			rc::retain(a);
			return a;
		}

		// Allocate a new GC string instance and concat within it.
		//
		uint32_t len = a->length + b->length;
		string*  str = L->alloc<string>(len + 1);
		str->length  = len;
		memcpy(str->data, a->data, a->length);
		memcpy(str->data + a->length, b->data, b->length + 1);
		return string_set::push_if(L, str);
	}
	string* string::concat(vm* L, any* a, slot_t n) {
		// Coerce all to string and compute total length.
		//
		uint32_t len = 0;
		for (slot_t i = 0; i < n; i++) {
			string* s;
			if (a[i].is_str()) {
				s = a[i].as_str();
			} else {
				s = a[i].coerce_str(L);
				rc::replace_adopt(L, a[i], any(s));
			}
			len += s->length;
		}

		// Handle empty case.
		//
		if (!len) [[unlikely]]
			return string::create(L);

		// Allocate a new GC string instance and concat within it.
		//
		string* str = L->alloc<string>(len + 1);
		str->length = len;
		char* it    = str->data;
		for (slot_t i = 0; i < n; i++) {
			string* s = a[i].as_str();
			memcpy(it, s->data, s->length);
			it += s->length;
		}
		*it++ = '\x0';
		return string_set::push_if(L, str);
	}

	void gc::destroy(vm* owner, string* o) {
		// Shared destruction supplies the process-wide allocator VM, never the
		// caller which released its last reference. Its global registry is
		// serialized with shared interning; private destruction stays lock-free.
		if (o->shared) {
			std::lock_guard guard(shared::heap_mutex());
			strset_remove(owner, o);
		} else {
			strset_remove(owner, o);
		}
	}
};