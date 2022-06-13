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
		type_table     = 0,
		type_userdata  = 1,  // Last traitful.
		type_array     = 2,
		type_function  = 3,
		type_proto     = 4,  // Not visible to user.
		type_string    = 5,
		type_bool      = 8,  // First non-GC type.
		type_nil       = 9,
		type_opaque    = 10,  // Not visible to user, unique integer part.
		type_exception = 11,  // Not visible to user, marker.
		type_number    = 12,  // Not a real enumerator, everything below this is also a number.

		// GC aliases.
		type_gc_last_traitful = type_userdata,
		type_gc_last          = 7,

		type_gc_free,
		type_gc_private,
		type_gc_uninit,
		type_gc_jfunc,

		// Invalid
		type_invalid = 0xFF
	};

	// Type names.
	//
	static constexpr std::array<const char*, 16> type_names = []() {
		std::array<const char*, 16> result;
		result.fill("invalid");
		result[type_table]     = "table";
		result[type_array]     = "array";
		result[type_function]  = "function";
		result[type_proto]     = "proto";
		result[type_string]    = "string";
		result[type_userdata]  = "userdata";
		result[type_nil]       = "nil";
		result[type_bool]      = "bool";
		result[type_opaque]    = "opaque";
		result[type_exception] = "exception";
		result[type_number]    = "number";
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
	LI_INLINE static gc::header* get_gc_value(uint64_t value) {
#if LI_KERNEL_MODE
		value |= ~0ull << 47;
#else
		value = mask_value(value);
#endif
		return (gc::header*) value;
	}

	// Type and value traits.
	//
	LI_INLINE static constexpr uint64_t get_type(uint64_t value) { return ((~value) >> 47); }
	template<value_type Type>
	LI_INLINE static constexpr bool is_value_of_type(uint64_t value) {
		if constexpr (Type == type_exception || Type == type_nil) {
			return value == make_tag(Type);
		} else if constexpr (Type == type_number) {
			constexpr uint32_t expected = uint32_t(make_tag(type_number + 1) >> 47);
			return (value >> 47) < expected;
		} else {
			constexpr uint32_t expected = uint32_t(make_tag(Type) >> 47);
			return (value >> 47) == expected;
		}
	}
	LI_INLINE static constexpr bool is_type_gc(uint8_t t) { return t <= type_gc_last; }
	LI_INLINE static constexpr bool is_type_traitful(uint8_t t) { return t <= type_gc_last_traitful; }
	LI_INLINE static constexpr bool is_value_gc(uint64_t value) { return value > (make_tag(type_gc_last + 1) + 1); }
	LI_INLINE static constexpr bool is_value_traitful(uint64_t value) { return value > (make_tag(type_gc_last_traitful + 1) + 1); }

	// Forward for auto-typing.
	//
	namespace gc {
		static value_type identify(const header* h);
	};

	// Boxed object type, fixed 8-bytes.
	//
	struct LI_TRIVIAL_ABI any {
		uint64_t value = make_tag(type_nil);

		// Literal construction.
		//
		LI_INLINE inline constexpr any(bool v) : value(mix_value(type_bool, v?1:0)) {}
		LI_INLINE inline constexpr any(number v) : value(li::bit_cast<uint64_t>(v)) {
			// TODO: Might be optimized out if compiled with -ffast-math.
			if (v != v) [[unlikely]]
				value = kvalue_nan;
		}
		LI_INLINE inline constexpr any(std::in_place_t, uint64_t value) : value(value) {}
		LI_INLINE inline constexpr any(opaque v) : value(mix_value(type_opaque, (uint64_t) v.bits)) {}

		// Trivially copyable and default constructable.
		//
		inline constexpr any()                          = default;
		inline constexpr any(any&&) noexcept            = default;
		inline constexpr any(const any&)                = default;
		inline constexpr any& operator=(any&&) noexcept = default;
		inline constexpr any& operator=(const any&)     = default;

		// GC types.
		//
		LI_INLINE inline any(array* v) : value(mix_value(type_array, (uint64_t) v)) {}
		LI_INLINE inline any(table* v) : value(mix_value(type_table, (uint64_t) v)) {}
		LI_INLINE inline any(string* v) : value(mix_value(type_string, (uint64_t) v)) {}
		LI_INLINE inline any(userdata* v) : value(mix_value(type_userdata, (uint64_t) v)) {}
		LI_INLINE inline any(function* v) : value(mix_value(type_function, (uint64_t) v)) {}
		LI_INLINE inline any(gc::header* v) : value(mix_value(gc::identify(v), (uint64_t) v)) {}

		// Type check.
		//
		LI_INLINE inline constexpr value_type type() const { return (value_type) std::min(get_type(value), (uint64_t) type_number); }

		template<value_type Type>
		LI_INLINE inline constexpr bool is() const {
			return is_value_of_type<Type>(value);
		}
		LI_INLINE inline constexpr bool is_num() const { return is<type_number>(); }
		LI_INLINE inline constexpr bool is_bool() const { return is<type_bool>(); }
		LI_INLINE inline constexpr bool is_arr() const { return is<type_array>(); }
		LI_INLINE inline constexpr bool is_tbl() const { return is<type_table>(); }
		LI_INLINE inline constexpr bool is_str() const { return is<type_string>(); }
		LI_INLINE inline constexpr bool is_udt() const { return is<type_userdata>(); }
		LI_INLINE inline constexpr bool is_fn() const { return is<type_function>(); }
		LI_INLINE inline constexpr bool is_opq() const { return is<type_opaque>(); }
		LI_INLINE inline constexpr bool is_exc() const { return is<type_exception>(); }
		LI_INLINE inline constexpr bool is_gc() const { return is_value_gc(value); }
		LI_INLINE inline constexpr bool is_traitful() const { return is_value_traitful(value); }

		// Getters.
		//
		LI_INLINE inline constexpr bool   as_bool() const { return value & 1; }
		LI_INLINE inline constexpr number as_num() const { return li::bit_cast<number>(value); }
		LI_INLINE inline constexpr opaque as_opq() const { return {.bits = mask_value(value)}; }
		LI_INLINE inline gc::header*      as_gc() const { return get_gc_value(value); }
		LI_INLINE inline array*           as_arr() const { return (array*) as_gc(); }
		LI_INLINE inline table*           as_tbl() const { return (table*) as_gc(); }
		LI_INLINE inline string*          as_str() const { return (string*) as_gc(); }
		LI_INLINE inline userdata*        as_udt() const { return (userdata*) as_gc(); }
		LI_INLINE inline function*        as_fn() const { return (function*) as_gc(); }

		// Bytewise equal comparsion.
		//
		LI_INLINE inline constexpr bool equals(const any& other) const {
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

		// Define copy and comparison operators.
		//
		LI_INLINE inline constexpr bool operator==(const any& other) const { return equals(other); }
		LI_INLINE inline constexpr bool operator!=(const any& other) const { return !equals(other); }

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
	static constexpr any exception_marker{std::in_place, make_tag(type_exception)};

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
		exc,
		tbl,  // same order as gc types
		udt,
		arr,
		fn,
		proto,
		str,

		// Instruction stream types.
		//
		bb,

		// Native function info.
		//
		nfni,

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
		else if (vt == type::exc)
			return type_exception;
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
		else if (t == type_exception)
			return type::exc;
		else if (t == type_nil)
			return type::nil;
		else if (t <= type_gc_last)
			return type(uint8_t(t) + uint8_t(type::tbl) - type_table);
		else
			assume_unreachable();
	}
};