#pragma once
#include <algorithm>
#include <bit>
#include <string>
#include <util/common.hpp>

#pragma pack(push, 1)
namespace li {
	struct vm;

	// Integral types.
	//
	using number = double;

	// Opaque types.
	//
	struct opaque {
		uint64_t bits : 47;
	};
	struct iopaque {
		uint64_t bits : 47;
	};

	// GC types (forward).
	//
	namespace gc {
		struct header;
	};
	struct header;
	struct array;
	struct table;
	struct string;
	struct userdata;
	struct function;
	struct nfunction;
	struct thread;

	// Type enumerator.
	//
	enum value_type : uint8_t /*:4*/ {
		type_none      = 0,  // <-- must be 0, memset(0xFF) = array of nulls
		type_false     = 1,  // <-- canonical type boolean
		type_true      = 2,
		type_array     = 3,
		type_table     = 4,
		type_string    = 5,
		type_userdata  = 6,
		type_function  = 7,
		type_nfunction = 8,
		type_thread    = 9,
		type_opaque    = 10,  // No type/definition, unique integer part.
		type_iopaque   = 11,  // - Internal version used by bytecode, user shouldn't be able to touch it, value is blindly trusted.
		type_number    = 12,
	};
	static constexpr const char* type_names[] = {"none", "bool", "bool", "array", "table", "string", "userdata", "function", "nfunction", "thread", "opaque", "iopaque", "number"};
	LI_INLINE static constexpr value_type to_canonical_type_name( value_type t ) {
		if (t == type_none)
			return type_table;
		if (t == type_true)
			return type_false;
		if (t == type_nfunction)
			return type_function;
		return t;
	}

	LI_INLINE static constexpr bool     is_gc_type(uint64_t type) { return type_array <= type && type <= type_thread; }
	LI_INLINE static constexpr uint64_t mask_value(uint64_t value) { return value & ((1ull << 47) - 1); }
	LI_INLINE static constexpr uint64_t mix_value(uint8_t type, uint64_t value) { return ((~uint64_t(type)) << 47) | mask_value(value); }
	LI_INLINE static constexpr uint64_t make_tag(uint8_t type) { return ((~uint64_t(type)) << 47) | mask_value(~0ull); }
	LI_INLINE static constexpr uint64_t get_type(uint64_t value) { return ((~value) >> 47); }
	LI_INLINE static gc::header*        get_gc_value(uint64_t value) {
		value = mask_value(value);
#if _KERNEL_MODE
		value |= ~0ull << 47;
#endif
		return (gc::header*) value;
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
		inline constexpr any(number v) : value(bit_cast<uint64_t>(v)) {
			if (v != v) [[unlikely]]
				value = kvalue_nan;
		}
		inline constexpr any(std::in_place_t, uint64_t value) : value(value) {}

		// GC types.
		//
		inline any(array* v) : value(mix_value(type_array, (uint64_t) v)) {}
		inline any(table* v) : value(mix_value(type_table, (uint64_t) v)) {}
		inline any(string* v) : value(mix_value(type_string, (uint64_t) v)) {}
		inline any(userdata* v) : value(mix_value(type_userdata, (uint64_t) v)) {}
		inline any(function* v) : value(mix_value(type_function, (uint64_t) v)) {}
		inline any(nfunction* v) : value(mix_value(type_nfunction, (uint64_t) v)) {}
		inline any(thread* v) : value(mix_value(type_thread, (uint64_t) v)) {}
		inline any(opaque v) : value(mix_value(type_opaque, (uint64_t) v.bits)) {}
		inline any(iopaque v) : value(mix_value(type_iopaque, (uint64_t) v.bits)) {}

		// Type check.
		//
		inline value_type type() const { return (value_type) std::min(get_type(value), (uint64_t) type_number); }
		inline bool       is(uint8_t t) const { return t == type(); }
		inline bool       is_gc() const { return is_gc_type(get_type(value)); }
		inline bool       is_bool() const { return get_type(value) == type_false || get_type(value) == type_true; }
		inline bool       is_num() const { return get_type(value) >= type_number; }
		inline bool       is_arr() const { return get_type(value) == type_array; }
		inline bool       is_tbl() const { return get_type(value) == type_table; }
		inline bool       is_str() const { return get_type(value) == type_string; }
		inline bool       is_udt() const { return get_type(value) == type_userdata; }
		inline bool       is_vfn() const { return get_type(value) == type_function; }
		inline bool       is_nfn() const { return get_type(value) == type_nfunction; }
		inline bool       is_thr() const { return get_type(value) == type_thread; }
		inline bool       is_opq() const { return get_type(value) == type_opaque; }
		inline bool       is_iopq() const { return get_type(value) == type_iopaque; }

		// Getters.
		//
		inline bool        as_bool() const { return get_type(value) > type_false; }  // Free coersion.
		inline number      as_num() const { return bit_cast<number>(value); }
		inline gc::header* as_gc() const { return get_gc_value(value); }
		inline array*      as_arr() const { return (array*) as_gc(); }
		inline table*      as_tbl() const { return (table*) as_gc(); }
		inline string*     as_str() const { return (string*) as_gc(); }
		inline userdata*   as_udt() const { return (userdata*) as_gc(); }
		inline function*   as_vfn() const { return (function*) as_gc(); }
		inline nfunction*  as_nfn() const { return (nfunction*) as_gc(); }
		inline thread*     as_thr() const { return (thread*) as_gc(); }
		inline opaque      as_opq() const { return {.bits = mask_value(value)}; }

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

		// String conversion.
		//
		string*     to_string(vm*) const;
		std::string to_string() const;
		void        print() const;

		// Type coercion.
		//
		string* coerce_str(vm* L) const { return to_string(L); }
		bool    coerce_bool() const { return as_bool(); }
		number  coerce_num() const;

		// Hasher.
		//
		inline uint32_t hash() const {
#if LI_32 || !LI_HAS_CRC
			uint64_t x = value;
			x ^= x >> 33U;
			x *= UINT64_C(0xff51afd7ed558ccd);
			x ^= x >> 33U;
			return (uint32_t)x;
#else
			uint64_t h = ~0;
			h          = _mm_crc32_u64(h, value);
			return uint32_t(h + 1) * 134775813;
#endif
		}
	};
	static_assert(sizeof(any) == 8, "Invalid any size.");

	// Constants.
	//
	static constexpr any none{};
	static constexpr any const_false{false};
	static constexpr any const_true{true};

	// Fills the any[] with nones.
	//
	static void fill_none(void* data, size_t count) { memset(data, 0xFF, count * 8); }

};
#pragma pack(pop)