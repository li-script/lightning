#include <util/common.hpp>
#include <vm/table.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/function.hpp>

namespace li {
	// Sparse string hasher.
	//
	static uint32_t sparse_hash(std::string_view v) {
		const char* str = v.data();
		uint32_t    len = ( uint32_t ) v.size();

#if LI_HAS_CRC
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
#else
		uint32_t a, b;
		uint32_t h = len ^ 0xd3cccc57;
		if (len >= 4) {
			a = *(const uint32_t*) (str);
			h ^= *(const uint32_t*) (str + len - 4);
			b = *(const uint32_t*) (str + (len >> 1) - 2);
			h ^= b;
			h -= std::rotl(b, 14);
			b += *(const uint32_t*) (str + (len >> 2) - 1);
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

	struct string_set : gc::node<string_set> {
		static constexpr size_t min_size = 512;
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
			return {it, end()}; // allow full load.
		}

		// GC enumerator.
		//
		void gc_traverse(gc::stage_context s) override {
			for (auto& k : *this) {
				if (k && !k->gc_tick(s, true)) {
					k = nullptr;
				}
			}
		}

		// Simpler implementation of the same algorithm as table with no fixed holder.
		//
		[[nodiscard]] string_set* push(vm* L, string* s) {
			string_set* ss = this;
			while (true) {
				for (auto& entry : ss->find(s->hash)) {
					if (!entry) {
						entry = s;
						return ss;
					}
				}
				ss = nextsize(L);
			}
			return ss;
		}
		[[nodiscard]] string_set* nextsize(vm* L) {
			size_t old_count = size();
			size_t new_count = old_count << 1;

			string_set* new_set = L->alloc<string_set>(sizeof(string*) * (new_count + overflow_factor));
			std::fill_n(new_set->entries, new_count + overflow_factor, nullptr);

			for (size_t i = 0; i != (old_count + overflow_factor); i++) {
				if (string* s = entries[i]) {
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
			for (auto& entry : L->str_intern->find(s->hash)) {
				if (entry && entry->view() == s->view()) {
					L->gc.free(L, s);
					return entry;
				}
			}

			// Push and return.
			//
			L->str_intern = L->str_intern->push(L, s);
			return s;
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

	// Internal string-set implementation.
	//
	void init_string_intern(vm* L) {
		L->str_intern = L->alloc<string_set>(sizeof(string*) * string_set::min_size);
		std::fill_n(L->str_intern->entries, string_set::min_size, nullptr);

		string* str = L->alloc<string>(1);
		str->data[0] = 0;
		str->length  = 0;
		str->hash    = 0;
		L->empty_string = str;
	}
	void traverse_string_set(vm* L, gc::stage_context s) { L->str_intern->gc_tick(s); }

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
		int  ns = vsnprintf(buffer, std::size(buffer), fmt, a2);
		va_end(a2);

		// If empty, handle.
		//
		if (ns <= 0) {
			return L->empty_string;
		}
		uint32_t n = uint32_t(ns);

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
		va_end(a1);
		return string_set::push_if(L, str);
	}
	string* string::concat( vm* L, string* a, string* b) {
		// Handle empty case.
		//
		if (a == b && a == L->empty_string) [[unlikely]]
			return a;

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
			string* s = a[i].coerce_str(L);
			len += s->length;
			a[i] = s;
		}

		// Handle empty case.
		//
		if (!len) [[unlikely]]
			return L->empty_string;

		// Allocate a new GC string instance and concat within it.
		//
		string*  str = L->alloc<string>(len + 1);
		str->length  = len;
		char* it = str->data;
		for (slot_t i = 0; i < n; i++) {
			string* s = a[i].as_str();
			memcpy(it, s->data, s->length);
			it += s->length;
		}
		*it++ = '\x0';
		return string_set::push_if(L, str);
	}


	// String coercion.
	//
	template<typename F>
	static void format_any(any a, F&& formatter) {
		switch (a.type()) {
			case type_none:
				formatter("{}");
				break;
			case type_false:
				formatter("false");
				break;
			case type_true:
				formatter("true");
				break;
			case type_number:
				if (a.as_num() == (int64_t) a.as_num()) {
					formatter("%.0lf", a.as_num());
				} else {
					formatter("%lf", a.as_num());
				}
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
				//formatter("function @ %p", a.as_gc());
				formatter("function @ %s", a.as_vfn()->src_chunk->c_str());
				break;
			case type_nfunction:
				formatter("nfunction @ %p", a.as_gc());
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
	string* any::to_string(vm* L) const {
		if (is_str()) [[likely]]
			return as_str();
		if (is_tbl()) {
			auto* tbl = as_tbl();
			if (tbl->has_trait<trait::str>()) {
				bool ok = L->scall(0, tbl->get_trait<trait::str>(), *this);
				auto res = L->pop_stack();
				if (ok && res.is_str()) {
					return res.as_str();
				}
			}
		}

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
		if (is_str())
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
		if (is_str()) [[likely]]
			return (void) fputs(as_str()->c_str(), stdout);
		format_any(*this, [&]<typename... Tx>(const char* fmt, Tx&&... args) {
			if constexpr (sizeof...(Tx) == 0) {
				fputs(fmt, stdout);
			} else {
				printf(fmt, std::forward<Tx>(args)...);
			}
		});
	}
	number any::coerce_num() const {
		if (is_num()) [[likely]]
			return as_num();
		switch (type()) {
			case type_none:
			case type_false:
				return 0;
			default:
				return 1;
			case type_string: {
				char* end;
				return strtod(as_str()->c_str(), &end);
			}
		}
	}
};