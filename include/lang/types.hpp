#pragma once
#include <intrin.h>
#include <stdint.h>
#include <cstring>
#include <util/common.hpp>
#include <intrin.h>

#pragma pack(push, 1)
namespace lightning::core {
	// Integral types.
	//
	using number  = double;
	using integer = int64_t;

	// Vector types.
	//
	struct vec2 {
		float x, y;
	};
	struct vec3 {
		float x, y, z;
	};

	// GC types (forward).
	//
	struct gc_header;
	struct array;
	struct table;
	struct string;
	struct userdata;
	struct function;
	struct thread;

	// Type enumerator.
	//
	enum value_type : uint8_t /*:4*/ {
		type_none     = 0, // <-- must be 0, memset(0) = array of nulls
		type_false    = 1,
		type_true     = 2,
		type_number   = 3,
		type_integer  = 4,
		type_vec2     = 5,
		type_vec3     = 6,
		type_gc       = 8,  // GC flag
		type_array    = type_gc | 0,
		type_table    = type_gc | 1,
		type_string   = type_gc | 2,
		type_userdata = type_gc | 3,
		type_function = type_gc | 4,
		type_thread   = type_gc | 5,
	};

	// Boxed object type, fixed 16-bytes.
	//
	struct LI_ALIGN(16) any {
		union {
			number     n;
			integer    i;
			vec2       v2;
			vec3       v3;
			array*     a;
			table*     t;
			string*    s;
			userdata*  u;
			function*  f;
			thread*    z;
			gc_header* gc;
		};
		uint8_t type;

		// Literal construction.
		//
		inline any() 
		{ 
#if _MSC_VER
			_mm_storeu_si128((__m128i*) this, _mm_setzero_si128());
#else
			memset(this, 0, 16);
#endif
		}
		inline any(bool v) : any() { type = v + type_false; }
		inline any(number v) : any() { type = type_number, n = v; }
		inline any(integer v) : any() { type = type_integer, i = v; }
		inline any(vec2 v) : any() { type = type_vec2, v2 = v; }
		inline any(vec3 v) : any() { type = type_vec3, v3 = v; }

		// GC types.
		//
		inline any(array* v) : any() { type = type_array, a = v; }
		inline any(table* v) : any() { type = type_table, t = v; }
		inline any(string* v) : any() { type = type_string, s = v; }
		inline any(userdata* v) : any() { type = type_userdata, u = v; }
		inline any(function* v) : any() { type = type_function, f = v; }
		inline any(thread* v) : any() { type = type_thread, z = v; }

		// Bytewise equal comparsion.
		//
		inline bool equals(const any& other) const {
#if _MSC_VER
			__m128i v = _mm_xor_si128(_mm_loadu_si128((__m128i*) this), _mm_loadu_si128((__m128i*) &other));
			return _mm_testz_si128(v, v);
#else
			return !memcmp(this, &other, 16);
#endif
		}

		// Bytewise copy.
		//
		inline void copy_from(const any& other) {
#if _MSC_VER
			_mm_storeu_si128((__m128i*) this, _mm_loadu_si128((__m128i*) &other));
#elif __has_builtin(__builtin_memcpy_inline)
			__builtin_memcpy_inline(this, &other, 16);
#else
			*this = other;
#endif
		}

		// Define copy and comparison operators.
		//
		inline any(const any& other) { copy_from(other); }
		inline any& operator=(const any& other) {
			copy_from(other);
			return *this;
		}
		inline bool operator==(const any& other) const { return equals(other); }
		inline bool operator!=(const any& other) const { return !equals(other); }

		// Hasher.
		//
		inline uint32_t hash() const {
			uint64_t h = ~0;
			h          = _mm_crc32_u64(h, ((uint64_t*) this)[0]);
			h          = _mm_crc32_u64(h, ((uint64_t*) this)[1]);
			return uint32_t(h + 1) * 134775813;
		}
	};
	static_assert(sizeof(any) == 16, "Invalid any size.");
};
#pragma pack(pop)