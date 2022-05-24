#pragma once
#include <intrin.h>
#include <stdint.h>
#include <cstring>
#include <util/common.hpp>

#pragma pack(push, 1)
namespace lightning::core {
	// Integral types.
	//
	using boolean = bool;
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
	struct vec4 {
		float x, y, z, w;
	};

	// GC types (forward).
	//
	struct array;
	struct table;
	struct string;
	struct userdata;

	// Type enumerator.
	//
	enum value_type : uint8_t /*:4*/ {
		value_none     = 0,
		value_true     = 1,
		value_false    = 2,
		value_number   = 3,
		value_integer  = 4,
		value_vec2     = 5,
		value_vec3     = 6,
		value_gc       = 8,  // GC flag
		value_array    = value_gc | 0,
		value_table    = value_gc | 1,
		value_string   = value_gc | 2,
		value_userdata = value_gc | 3,
	};

	// Boxed object type, fixed 16-bytes.
	//
	struct LI_ALIGN(16) any {
		union {
			number    n;
			integer   i;
			vec2      v2;
			vec3      v3;
			array*    a;
			table*    t;
			string*   s;
			userdata* u;
		};
		uint32_t type;

		// Literal construction.
		//
		inline constexpr any() : i(0), type(value_none) {}
		inline constexpr any(bool v) : i(0), type(v ? value_true : value_false) {}
		inline constexpr any(number v) : n(v), type(value_number) {}
		inline constexpr any(integer v) : i(v), type(value_integer) {}
		inline constexpr any(vec2 v) : v2(v), type(value_vec2) {}
		inline constexpr any(vec3 v) : v3(v), type(value_vec3) {}

		// GC types.
		//
		inline constexpr any(array* v) : a(v), type(value_array) {}
		inline constexpr any(table* v) : t(v), type(value_table) {}
		inline constexpr any(string* v) : s(v), type(value_string) {}
		inline constexpr any(userdata* v) : u(v), type(value_userdata) {}

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
	};
	static_assert(sizeof(any) == 16, "Invalid any size.");
};
#pragma pack(pop)