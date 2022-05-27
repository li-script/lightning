#include <vm/table.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>

namespace li {
	// Sparse string hasher.
	//
	static uint32_t sparse_hash(std::string_view v) {
		const char* str = v.data();
		uint32_t    len = ( uint32_t ) v.size();

		uint32_t crc = len;
		if (len >= 4) {
			crc = _mm_crc32_u32(crc, *(const uint32_t*) (str));
			crc = _mm_crc32_u32(crc, *(const uint32_t*) (str + len - 4));
			crc = _mm_crc32_u32(crc, *(const uint32_t*) (str + (len >> 1) - 2));
			crc = _mm_crc32_u32(crc, *(const uint32_t*) (str + (len >> 2) - 1));
		} else {
			crc = _mm_crc32_u8(crc, *(const uint8_t*) str);
			crc = _mm_crc32_u8(crc, *(const uint8_t*) (str + len - 1));
			crc = _mm_crc32_u8(crc, *(const uint8_t*) (str + (len >> 1)));
		}
		return crc;
	}

	// TODO: Weak node?
	struct string_set : gc::node<string_set> {
		static constexpr size_t overflow_factor = 8;

		string* entries[];

		// Expose the same interface as table.
		//
		size_t             size() { return (this->object_bytes() / sizeof(string*)) - overflow_factor; }
		size_t             mask() { return size() - 1; }
		string**           begin() { return &entries[0]; }
		string**           end() { return &entries[size() + overflow_factor]; }
		std::span<string*> find(size_t hash) {
			auto it = begin() + (hash & mask());
			return {it, it + overflow_factor};
		}

		// GC enumerator.
		//
		void gc_traverse(gc::sweep_state& s) override {
			for (auto& k : *this) {
				if (k && !k->gc_tick(s, true)) {
					k = nullptr;
				}
			}
		}

		// Simpler implementation of the same algorithm as table with no fixed holder.
		//
		[[nodiscard]] string_set* push(vm* L, string* s, bool assert_no_resize = false) {
			string_set* ss = this;
			while (true) {
				for (auto& entry : ss->find(s->hash)) {
					if (!entry) {
						entry = s;
						return ss;
					}
				}
				LI_ASSERT(!assert_no_resize);
				ss = nextsize(L);
			}
			return ss;
		}
		[[nodiscard]] string_set* nextsize(vm* L) {
			size_t old_count = size();
			size_t new_count = old_count << 1;

			string_set* new_set = L->alloc<string_set>(sizeof(string*) * (new_count + overflow_factor));
			std::fill_n(new_set->entries, new_count + overflow_factor, nullptr);

			for (size_t i = 0; i != old_count; i++) {
				if (string* s = entries[i]) {
					(void) new_set->push(L, s, true);
				}
			}
			return new_set;
		}
		static string* push(vm* L, std::string_view key) {
			if (key.empty()) [[unlikely]] {
				return L->empty_string;
			}

			uint32_t hash = sparse_hash(key);

			// Return if already exists.
			//
			for (auto& entry : L->str_intern->find(hash)) {
				if (entry && entry->view() == key)
					return entry;
			}

			// Create and push otherwise.
			//
			string* str = L->alloc<string>(key.size() + 1);
			memcpy(str->data, key.data(), key.size());
			str->data[key.size()] = 0;
			str->length           = (uint32_t) key.size();
			str->hash             = hash;
			L->str_intern         = L->str_intern->push(L, str);
			return str;
		}
	};

	
	static constexpr size_t min_size = 512;

	// Internal string-set implementation.
	//
	void init_string_intern(vm* L) {
		L->str_intern = L->alloc<string_set>(sizeof(string*) * min_size);
		std::fill_n(L->str_intern->entries, min_size, nullptr);

		string* str = L->alloc<string>(1);
		str->data[0] = 0;
		str->length  = 0;
		str->hash    = 0;
		L->empty_string = str;
	}
	void traverse_string_set(vm* L, gc::sweep_state& s) { L->str_intern->gc_traverse(s); }

	// String creation.
	//
	string* string::create(vm* L, std::string_view from) {
		return string_set::push(L, from);
	}
	string* string::format( vm* L, const char* fmt, ... ) {
		va_list a1;
		va_start(a1, fmt);
		
		// First try formatting on stack:
		//
		va_list a2;
		va_copy(a2, a1);
		char buffer[64];
		int  n = vsnprintf(buffer, std::size(buffer), fmt, a2);
		va_end(a2);

		// If empty, handle.
		//
		if (n <= 0) {
			return L->empty_string;
		}

		// If it did fit, forward to string::create with a view:
		//
		if (n <= std::size(buffer)) {
			va_end(a1);
			return string::create(L, {buffer, (size_t)n});
		}

		// Otherwise, allocate a string and format into it.
		//
		string* str = L->alloc<string>(n + 1);
		str->length = n;
		vsnprintf(str->data, n, fmt, a1);
		str->hash = sparse_hash(str->view());
		va_end(a1);
		
		// Return if already exists.
		//
		for (auto& entry : L->str_intern->find(str->hash)) {
			if (entry && entry->view() == str->view()) {
				// TODO: Free string?
				return entry;
			}
		}

		// Push and return.
		//
		L->str_intern = L->str_intern->push(L, str);
		return str;
	}

	string* string::concat( vm* L, string* a, string* b) {
		// TODO: lol
		return string::format(L, "%s%s", a->data, b->data);
	}

	// String coercion.
	//
	template<typename F>
	static void format_any( any a, F&& formatter ) {
		switch (a.type()) {
			case type_none:
				formatter("None");
				break;
			case type_false:
				formatter("false");
				break;
			case type_true:
				formatter("true");
				break;
			case type_number:
				formatter("%lf", a.as_num());
				break;
			case type_array:
				formatter("array @ %p", a.as_gc());
				break;
			case type_table:
				formatter("table @ %p", a.as_gc());
				break;
			case type_string:
				formatter("\"%s\"", a.as_str()->data);
				break;
			case type_userdata:
				formatter("userdata @ %p", a.as_gc());
				break;
			case type_function:
				formatter("function @ %p", a.as_gc());
				break;
			case type_nfunction:
				formatter("Nfunction @ %p", a.as_gc());
				break;
			case type_opaque:
				formatter("opaque %llx", (uint64_t) a.as_opq().bits);
				break;
			case type_iopaque:
				formatter("iopaque %llx", (uint64_t) a.as_opq().bits);
				break;
			case type_thread:
				formatter("thread @ %p", a.as_gc());
				break;
			default:
				util::abort("invalid type");
				break;
		}
	};
	string*     any::to_string(vm* L) const {
		if (type() == type_string)
			return as_str();
		string* result;
		format_any(*this, [&] <typename... Tx> (const char* fmt, Tx&&... args) {
			if constexpr (sizeof...(Tx) == 0) {
				result = string::create(L, fmt);
			} else {
				result = string::format(L, fmt, std::forward<Tx>(args)...);
			}
		});
		return result;
	}
	std::string any::to_string() const {
		if (type() == type_string)
			return std::string{as_str()->view()};
		std::string result;
		format_any(*this, [&]<typename... Tx>(const char* fmt, Tx&&... args) {
			if constexpr (sizeof...(Tx) == 0) {
				result = fmt;
			} else {
				result = util::fmt(fmt, std::forward<Tx>(args)...);
			}
		});
		return result;
	}
	void any::print() const {
		format_any(*this, [&]<typename... Tx>(const char* fmt, Tx&&... args) {
			printf(fmt, std::forward<Tx>(args)...);
		});
	}
};