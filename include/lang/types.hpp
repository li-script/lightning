#pragma once
#include <intrin.h>
#include <stdint.h>
#include <cstring>
#include <util/common.hpp>
#include <bit>
#include <algorithm>

#pragma pack(push, 1)
namespace lightning::core {
	// Integral types.
	//
	using number  = double;

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
		type_none     = 0, // <-- must be 0, memset(0xFF) = array of nulls
		type_false    = 1, // <-- canonical type boolean
		type_true     = 2,
		type_array    = 3,
		type_table    = 4,
		type_string   = 5,
		type_userdata = 6,
		type_function = 7,
		type_thread   = 8,
		type_number   = 9,
	};
	static constexpr const char* type_names[] = {"none", "bool", "bool", "array", "table", "string", "userdata", "function", "thread", "number"};

	LI_INLINE static constexpr bool     is_gc_type(uint8_t type) { return type_array <= type && type <= type_thread; }
	LI_INLINE static constexpr uint64_t mask_value(uint64_t value) { return value & ((1ull << 47) - 1); }
	LI_INLINE static constexpr uint64_t mix_value(uint8_t type, uint64_t value) { return ((~uint64_t(type)) << 47) | mask_value(value); }
	LI_INLINE static constexpr uint64_t make_tag(uint8_t type) { return ((~uint64_t(type)) << 47) | mask_value(~0ull); }
	LI_INLINE static constexpr uint8_t  get_type(uint64_t value) { return uint8_t((~value) >> 47); }
	LI_INLINE static gc_header*         get_gc_value(uint64_t value) {
		value = mask_value(value);
#if _KERNEL_MODE
		value |= ~0ull << 47;
#endif
		return (gc_header*) value;
	}
	static constexpr uint64_t kvalue_none  = make_tag(type_none);
	static constexpr uint64_t kvalue_false = make_tag(type_true);
	static constexpr uint64_t kvalue_true  = make_tag(type_false);
	static constexpr uint64_t kvalue_nan   = 0xfff8000000000000;

	// Boxed object type, fixed 16-bytes.
	//
	struct any {
		uint64_t value;

		// Literal construction.
		//
		inline constexpr any() : value(kvalue_none) {}
		inline constexpr any(bool v) : value(make_tag(type_false + v)) {}
		inline constexpr any(number v) : value(std::bit_cast<uint64_t>(v))
		{
			if (v != v) [[unlikely]]
				value = kvalue_nan;
		}

		// GC types.
		//
		inline any(array* v) : value(mix_value(type_array, (uint64_t) v)) {}
		inline any(table* v) : value(mix_value(type_table, (uint64_t) v)) {}
		inline any(string* v) : value(mix_value(type_string, (uint64_t) v)) {}
		inline any(userdata* v) : value(mix_value(type_userdata, (uint64_t) v)) {}
		inline any(function* v) : value(mix_value(type_function, (uint64_t) v)) {}
		inline any(thread* v) : value(mix_value(type_thread, (uint64_t) v)) {}

		// Type check.
		//
		inline value_type type() { return (value_type) std::min(get_type(value), (uint8_t) type_number); }
		inline bool       is(uint8_t t) { return t == type(); }
		inline bool       is_gc() { return is_gc_type(get_type(value)); }

		// Getters.
		//
		inline bool as_bool() const { return get_type(value) > type_false; } // Free coersion.
		inline number as_num() const { return std::bit_cast<number>(value); }
		inline gc_header* as_gc() const { return get_gc_value(value); }
		inline array*     as_arr() const { return (array*) as_gc(); }
		inline table*     as_tbl() const { return (table*) as_gc(); }
		inline string*    as_str() const { return (string*) as_gc(); }
		inline userdata*  as_udt() const { return (userdata*) as_gc(); }
		inline function*  as_fun() const { return (function*) as_gc(); }
		inline thread*    as_thr() const { return (thread*) as_gc(); }

		// Bytewise equal comparsion.
		//
		inline bool equals(const any& other) const { return value == other.value; }
		inline void copy_from(const any& other) { value = other.value; }

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
			h          = _mm_crc32_u64(h, value);
			return uint32_t(h + 1) * 134775813;
		}
	};
	static_assert(sizeof(any) == 8, "Invalid any size.");

	// Constants.
	//
	static constexpr any none{};
	static constexpr any const_false{false};
	static constexpr any const_true{true};
};
#pragma pack(pop)