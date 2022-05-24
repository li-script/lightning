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
	struct function;
	struct thread;

	// Type enumerator.
	//
	enum value_type : uint8_t /*:4*/ {
		type_none     = 0,
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
			number    n;
			integer   i;
			vec2      v2;
			vec3      v3;
			array*    a;
			table*    t;
			string*   s;
			userdata* u;
			function* f;
			thread*   v;
		};
		uint32_t type;

		// Literal construction.
		//
		inline constexpr any() : i(0), type(type_none) {}
		inline constexpr any(bool v) : i(0), type(v + type_false) {}
		inline constexpr any(number v) : n(v), type(type_number) {}
		inline constexpr any(integer v) : i(v), type(type_integer) {}
		inline constexpr any(vec2 v) : v2(v), type(type_vec2) {}
		inline constexpr any(vec3 v) : v3(v), type(type_vec3) {}

		// GC types.
		//
		inline constexpr any(array* v) : a(v), type(type_array) {}
		inline constexpr any(table* v) : t(v), type(type_table) {}
		inline constexpr any(string* v) : s(v), type(type_string) {}
		inline constexpr any(userdata* v) : u(v), type(type_userdata) {}
		inline constexpr any(function* v) : f(v), type(type_function) {}
		inline constexpr any(thread* v) : v(v), type(type_thread) {}

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