#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <string>
#include <util/common.hpp>

#pragma pack(push, 1)
namespace li {
	struct vm;
	using slot_t = intptr_t;

	// Integral types.
	//
	using number = double;

	// Opaque types.
	//
	struct opaque {
		uint64_t bits : 47;
		uint64_t rsvd : 17;
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

	// Type enumerator.
	//
	enum value_type : uint8_t /*:4*/ {
		// - first gc type
		type_table    = 0,
		type_userdata = 1,  // Last traitful.
		type_array    = 2,
		type_function = 3,
		type_proto    = 4,  // Last traversable. | Not visible to user.
		type_string   = 5,
		type_bool     = 8,  // First non-GC type.
		type_nil      = 9,
		type_opaque   = 10,  // No type/definition, unique integer part.
		type_number   = 11,

		// GC aliases.
		type_gc_free = type_bool,
		type_gc_private,
		type_gc_uninit,
		type_gc_last             = 7,
		type_gc_last_traversable = type_proto,
		type_gc_last_traitful    = type_userdata,

		// Invalid
		type_invalid = 0xFF
	};

	// Type names.
	//
	static constexpr std::array<const char*, 16> type_names = []() {
		std::array<const char*, 16> result;
		result.fill("invalid");
		result[type_table]    = "table";
		result[type_array]    = "array";
		result[type_function] = "function";
		result[type_proto]    = "proto";
		result[type_string]   = "string";
		result[type_userdata] = "userdata";
		result[type_nil]      = "nil";
		result[type_bool]     = "bool";
		result[type_opaque]   = "opaque";
		result[type_number]   = "number";
		return result;
	}();

	// NaN boxing details.
	//
	static constexpr uint64_t           kvalue_nan = 0xfff8000000000000;
	LI_INLINE static constexpr uint64_t mask_value(uint64_t value) { return value & util::fill_bits(47); }
	LI_INLINE static constexpr uint64_t mix_value(uint8_t type, uint64_t value)
	{
#if LI_KERNEL_MODE
		value = mask_value(value);
#endif
		return ((~uint64_t(type)) << 47) | value;
	}
	LI_INLINE static constexpr uint64_t make_tag(uint8_t type) { return ((~uint64_t(type)) << 47) | mask_value(~0ull); }
	LI_INLINE static constexpr uint64_t get_type(uint64_t value) { return ((~value) >> 47); }
	LI_INLINE static gc::header*        get_gc_value(uint64_t value) {
		value = mask_value(value);
#if LI_KERNEL_MODE
		value |= ~0ull << 47;
#endif
		return (gc::header*) value;
	}

	// Type and value traits.
	//
	LI_INLINE static constexpr bool is_type_gc(uint8_t t) { return t <= type_gc_last; }
	LI_INLINE static constexpr bool is_type_traitful(uint8_t t) { return t <= type_gc_last_traitful; }
	LI_INLINE static constexpr bool is_type_gc_traversable(uint8_t t) { return t <= type_gc_last_traversable; }
	LI_INLINE static constexpr bool is_gc_value(uint64_t value) { return value > (make_tag(type_gc_last + 1) + 1); }
	LI_INLINE static constexpr bool is_traitful_value(uint64_t value) { return value > (make_tag(type_gc_last_traitful + 1) + 1); }
	LI_INLINE static constexpr bool is_gc_traversable_value(uint64_t value) { return value > (make_tag(type_gc_last_traversable + 1) + 1); }
	LI_INLINE static constexpr bool is_numeric_value(uint64_t value) { return value <= make_tag(type_number); }

	// Forward for auto-typing.
	//
	namespace gc {
		static value_type identify(const header* h);
	};

	// Boxed object type, fixed 8-bytes.
	//
	struct LI_TRIVIAL_ABI any {
		uint64_t value;

		// Literal construction.
		//
		inline constexpr any() : value(make_tag(type_nil)) {}
		inline constexpr any(bool v) : value(mix_value(type_bool, v?1:0)) {}
		inline constexpr any(number v) : value(li::bit_cast<uint64_t>(v)) {
			if (v != v) [[unlikely]]
				value = kvalue_nan;
		}
		inline constexpr any(std::in_place_t, uint64_t value) : value(value) {}
		inline constexpr any(opaque v) : value(mix_value(type_opaque, (uint64_t) v.bits)) {}

		// GC types.
		//
		inline any(array* v) : value(mix_value(type_array, (uint64_t) v)) {}
		inline any(table* v) : value(mix_value(type_table, (uint64_t) v)) {}
		inline any(string* v) : value(mix_value(type_string, (uint64_t) v)) {}
		inline any(userdata* v) : value(mix_value(type_userdata, (uint64_t) v)) {}
		inline any(function* v) : value(mix_value(type_function, (uint64_t) v)) {}
		inline any(gc::header* v) : value(mix_value(gc::identify(v), (uint64_t) v)) {}

		// Type check.
		//
		inline constexpr value_type type() const { return (value_type) std::min(get_type(value), (uint64_t) type_number); }
		inline constexpr bool       is(uint8_t t) const { return t == type(); }
		inline constexpr bool       is_bool() const { return get_type(value) == type_bool; }
		inline constexpr bool       is_num() const { return is_numeric_value(value); }
		inline constexpr bool       is_arr() const { return get_type(value) == type_array; }
		inline constexpr bool       is_tbl() const { return get_type(value) == type_table; }
		inline constexpr bool       is_str() const { return get_type(value) == type_string; }
		inline constexpr bool       is_udt() const { return get_type(value) == type_userdata; }
		inline constexpr bool       is_fn() const { return get_type(value) == type_function; }
		inline constexpr bool       is_opq() const { return get_type(value) == type_opaque; }
		inline constexpr bool       is_gc() const { return is_gc_value(value); }
		inline constexpr bool       is_traitful() const { return is_traitful_value(value); }

		// Getters.
		//
		inline constexpr bool   as_bool() const { return value & 1; }
		inline constexpr number as_num() const { return li::bit_cast<number>(value); }
		inline constexpr opaque as_opq() const { return {.bits = mask_value(value)}; }
		inline gc::header*      as_gc() const { return get_gc_value(value); }
		inline array*           as_arr() const { return (array*) as_gc(); }
		inline table*           as_tbl() const { return (table*) as_gc(); }
		inline string*          as_str() const { return (string*) as_gc(); }
		inline userdata*        as_udt() const { return (userdata*) as_gc(); }
		inline function*        as_fn() const { return (function*) as_gc(); }

		// Bytewise equal comparsion.
		//
		inline constexpr bool equals(const any& other) const {
#if !LI_FAST_MATH
			uint64_t x = value ^ other.value;
			if (!(value << 1)) {
				x <<= 1;
			}
			return x == 0 && value != kvalue_nan;
#else
			return value == other.value;
#endif
		}
		inline constexpr void copy_from(const any& other) { value = other.value; }

		// Define copy and comparison operators.
		//
		inline constexpr any(const any& other) { copy_from(other); }
		inline constexpr any& operator=(const any& other) {
			copy_from(other);
			return *this;
		}
		inline constexpr bool operator==(const any& other) const { return equals(other); }
		inline constexpr bool operator!=(const any& other) const { return !equals(other); }

		// String conversion.
		//
		string*     to_string(vm*) const;
		std::string to_string() const;
		void        print() const;

		// Type coercion.
		//
		string* coerce_str(vm* L) const { return to_string(L); }
		bool    coerce_bool() const { return value != mix_value(type_bool, 0) && value != make_tag(type_nil); }
		number  coerce_num() const;

		// Hasher.
		//
		inline size_t hash() const {
#if LI_32 || !LI_HAS_CRC
			uint64_t x = value;
			x ^= x >> 33;
			x *= 0xff51afd7ed558ccdull;
			x ^= x >> 33;
			return (size_t) x;
#else
			uint64_t h = value >> 8;
			return (size_t) _mm_crc32_u64(h, value);
#endif
		}
	};
	static_assert(sizeof(any) == 8, "Invalid any size.");

	// Constants.
	//
	static constexpr any nil{};
	static constexpr any const_false{false};
	static constexpr any const_true{true};

	// Fills the any[] with nones.
	//
	static void fill_nil(void* data, size_t count) {
		std::fill_n((uint64_t*) data, count, make_tag(type_nil));
	}
};
#pragma pack(pop)

// Define IR types here as it's used in native builtins regardless of whether or not IR is even used.
//
namespace li::ir {
	// Value types.
	//
	enum class type : uint8_t {
		none,

		// Integers.
		//
		i1,
		i8,
		i16,
		i32,
		i64,

		// Floating point types.
		//
		f32,
		f64,

		// Wrapped type (li::any).
		//
		unk,

		// VM types.
		//
		nil,
		opq,
		tbl,  // same order as gc types
		udt,
		arr,
		fn,
		proto,
		str,

		// Instruction stream types.
		//
		bb,

		// Enums.
		//
		vmopr,
		vmtrait,
		vmtype,
		irtype,

		// Aliases.
		//
		ptr = i64,
	};

	// Forwards.
	//
	struct value;
	struct insn;
	struct constant;
	struct basic_block;
	struct procedure;
	struct builder;

	// Conversion between IR and VM types.
	//
	static constexpr value_type to_vm_type(type vt) {
		if (vt == type::i1)
			return type_bool;
		else if (type::i8 <= vt && vt <= type::i64)
			return type_number;
		else if (type::f32 <= vt && vt <= type::f64)
			return type_number;
		else if (vt == type::nil)
			return type_nil;
		else if (vt == type::opq)
			return type_opaque;
		else if (type::tbl <= vt && vt < type::bb)
			return value_type(uint8_t(vt) - uint8_t(type::tbl) + type_table);
		else
			assume_unreachable();
	}
	static constexpr type to_ir_type(value_type t) {
		if (t == type_bool)
			return type::i1;
		else if (t == type_number)
			return type::f64;
		else if (t == type_opaque)
			return type::opq;
		else if (t == type_nil)
			return type::nil;
		else if (t <= type_gc_last)
			return type(uint8_t(t) + uint8_t(type::tbl) - type_table);
		else
			assume_unreachable();
	}
};