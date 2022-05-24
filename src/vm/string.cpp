#include <vm/table.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>

namespace lightning::core {
	// Sparse string hasher.
	//
	static uint32_t sparse_hash(std::string_view v) {
		const char* str = v.data();
		uint32_t    len = ( uint32_t ) v.size();

		uint32_t crc = 0xc561e88e - len;
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
		return crc + len;
	}

	struct string_set : gc_node<string_set> {
		static constexpr size_t overflow_factor = 8;

		string* entries[];

		// Expose the same interface as table.
		//
		size_t             size() { return (this->object_bytes() / sizeof(string*)) - overflow_factor; }
		size_t             mask() { return size() - 1; }
		string**           begin() { return &entries[0]; }
		string**           end() { return &entries[size()]; }
		std::span<string*> find(size_t hash) {
			auto it = begin() + (hash & mask());
			return {it, it + overflow_factor};
		}

		// GC enumerator.
		//
		template<typename F>
		void enum_for_gc(F&& fn) {
			for (auto& k : *this) {
				if (k) {
					fn(k);
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
			for (size_t i = 0; i != old_count; i++) {
				if (string* s = entries[i]) {
					(void) new_set->push(L, s, true);
				}
			}
			return new_set;
		}
		static string* push(vm* L, std::string_view key) {
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
	string_set* create_string_set(vm* L) {
		return L->alloc<string_set>(sizeof(string*) * 512);
	}

	// String creation.
	//
	string* string::create(vm* L, std::string_view from) {
		return string_set::push(L, from);
	}
};