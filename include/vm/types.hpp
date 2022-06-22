#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <string>
#include <util/common.hpp>

#ifndef LI_STRICT_ALIGN
	#if LI_ARCH_ARM
		#define LI_STRICT_ALIGN 1
	#else
		#define LI_STRICT_ALIGN 0
	#endif
#endif

// Forwards.
//
namespace li {
	namespace ir {
		struct value;
		struct insn;
		struct constant;
		struct basic_block;
		struct procedure;
		struct builder;
	};
	namespace gc {
		struct header;
	};
	struct vm;
	struct header;
	struct array;
	struct table;
	struct string;
	struct function;
	struct jfunction;
	struct function_proto;
	struct object;
	struct vclass;
	struct string_set;
};

namespace li {
	// Integral types.
	//
	using number = double;
	using slot_t = intptr_t;

	// Type enumerators.
	//
	enum value_type : uint8_t /*:4*/ {
		type_object     = 0,   // GC: Class, further divides into user types (<0 type id in gc header).
		type_table      = 1,   // GC: Table.
		type_array      = 2,   // GC: Array.
		type_function   = 3,   // GC: Function.
		type_string     = 4,   // GC: String.
		type_class      = 5,   // GC: Class type.
		type_bool       = 8,   // LI: Boolean | Literals.
		type_nil        = 9,   // LI: Nil tag.
		type_exception  = 10,  // LI: Exception tag. Not visible to user.
		type_number     = 11,  // LI: Double values. Not a real enumerator, everything below (if boxed) is also a number.
		type_gc_private = 12,  // AL: Private object. | GC aliases, only used in the GC header. Cannot be converted to any.
		type_gc_jfunc   = 13,  // AL: JIT function.
		type_gc_proto   = 14,  // AL: Function prototype.
		type_invalid    = 15,  // End of builtin types.

		// Pseudo indices.
		//
		type_gc_last = 7,
		type_gc_free = type_nil,
	};
	enum class type : int32_t {
		// -- Everything <0 is a user type.

		// Map value types one to one.
		//
		obj  = type_object,
		tbl  = type_table,
		arr  = type_array,
		fn   = type_function,
		str  = type_string,
		vcl  = type_class,
		i1   = type_bool,
		nil  = type_nil,
		exc  = type_exception,
		f64  = type_number,
		none = type_invalid,  // type_invalid == void.
		any,                  // Unknown type.

		// Numeric types that are not represented as a boxed value.
		//
		i8,
		i16,
		i32,
		i64,
		f32,

		// Private types.
		//
		bb,
		nfni,
		vmopr,
		vty,
		dty,

		// Aliases.
		//
		ptr = i64,
	};
	static constexpr bool is_integer_data(type t) { return type::i8 <= t && t <= type::i64; }
	static constexpr bool is_floating_point_data(type t) { return t == type::f32 || t == type::f64; }
	static constexpr bool is_marker_data(type t) { return t == type::nil || t == type::exc; }
	static constexpr bool is_gc_data(type t) { return t <= type::vcl; }
	static constexpr msize_t size_of_data(type t) {
		if (t == type::i8)
			return 1;
		if (t == type::i16)
			return 2;
		if (t == type::i32 || t == type::f32)
			return 4;
		return 8;
	}
	static constexpr msize_t align_of_data(type t) {
#if LI_STRICT_ALIGN
		return size_of_data(t);
#else
		return 1;
#endif
	}

	// Conversion between data and value types.
	//
	static constexpr value_type to_value_type(type vt) {
		if (vt <= type::obj)
			return type_object;
		if (vt <= type::f64)
			return value_type(vt);
		else if (is_floating_point_data(vt) || is_integer_data(vt))
			return type_number;
		else
			assume_unreachable();
	}
	static constexpr type to_type(value_type t) {
		if (t > type_number)
			assume_unreachable();
		return type(t);
	}

	// Type names.
	//
	static constexpr std::array<const char*, 16> type_names = []() {
		std::array<const char*, 16> result = {};
		result.fill("invalid");
		result[type_table]      = "table";
		result[type_array]      = "array";
		result[type_function]   = "function";
		result[type_string]     = "string";
		result[type_object]     = "object";
		result[type_class]      = "class";
		result[type_nil]        = "nil";
		result[type_bool]       = "bool";
		result[type_exception]  = "exception";
		result[type_number]     = "number";
		return result;
	}();
	std::string_view get_type_name(vm* L, type vt);

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
	LI_INLINE static constexpr bool is_value_gc(uint64_t value) { return value > (make_tag(type_gc_last + 1) + 1); }

	// Forward for auto-typing.
	//
	namespace gc {
		static value_type identify_value_type(const header* h);
	};

	// Boxed object type, fixed 8-bytes.
	//  any_t with no constructors is needed to make sure it passes in register in MS-ABI.
	//
	struct LI_TRIVIAL_ABI any_t {
		uint64_t value = make_tag(type_nil);

		// Type check.
		//
		LI_INLINE inline constexpr value_type type() const {
			return (value_type) std::min(get_type(value), (uint64_t) type_number);
		}
		template<value_type Type>
		LI_INLINE inline constexpr bool is() const {
			return is_value_of_type<Type>(value);
		}
		LI_INLINE inline constexpr bool is_num() const { return is<type_number>(); }
		LI_INLINE inline constexpr bool is_bool() const { return is<type_bool>(); }
		LI_INLINE inline constexpr bool is_arr() const { return is<type_array>(); }
		LI_INLINE inline constexpr bool is_tbl() const { return is<type_table>(); }
		LI_INLINE inline constexpr bool is_str() const { return is<type_string>(); }
		LI_INLINE inline constexpr bool is_obj() const { return is<type_object>(); }
		LI_INLINE inline constexpr bool is_vcl() const { return is<type_class>(); }
		LI_INLINE inline constexpr bool is_fn() const { return is<type_function>(); }
		LI_INLINE inline constexpr bool is_exc() const { return is<type_exception>(); }
		LI_INLINE inline constexpr bool is_gc() const { return is_value_gc(value); }

		// Full type getter and type namer.
		//
		const char* type_name() const;
		li::type xtype() const;

		// Getters.
		//
		LI_INLINE inline constexpr bool   as_bool() const { return value & 1; }
		LI_INLINE inline constexpr number as_num() const { return li::bit_cast<number>(value); }
		LI_INLINE inline gc::header*      as_gc() const { return get_gc_value(value); }
		LI_INLINE inline array*           as_arr() const { return (array*) as_gc(); }
		LI_INLINE inline table*           as_tbl() const { return (table*) as_gc(); }
		LI_INLINE inline string*          as_str() const { return (string*) as_gc(); }
		LI_INLINE inline vclass*          as_vcl() const { return (vclass*) as_gc(); }
		LI_INLINE inline object*          as_obj() const { return (object*) as_gc(); }
		LI_INLINE inline function*        as_fn() const { return (function*) as_gc(); }

		// Bytewise equal comparsion.
		//
		LI_INLINE inline constexpr bool equals(const any_t& other) const {
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

		// Define comparison operators.
		//
		LI_INLINE inline constexpr bool operator==(const any_t& other) const { return equals(other); }
		LI_INLINE inline constexpr bool operator!=(const any_t& other) const { return !equals(other); }

		// String conversion.
		//
		string*     to_string(vm*) const;
		std::string to_string() const;
		void        print() const;

		// Duplication.
		//
		any_t duplicate(vm* L) const;

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
	struct LI_TRIVIAL_ABI any : any_t {
		// Trivially copyable and default constructable.
		//
		inline constexpr any()                          = default;
		inline constexpr any(any&&) noexcept            = default;
		inline constexpr any(const any&)                = default;
		inline constexpr any& operator=(any&&) noexcept = default;
		inline constexpr any& operator=(const any&)     = default;

		// Literal construction.
		//
		LI_INLINE inline constexpr any(bool v) : any_t{mix_value(type_bool, v?1:0)} {}
		LI_INLINE inline constexpr any(number v) : any_t{li::bit_cast<uint64_t>(v)} {
			// TODO: Might be optimized out if compiled with -ffast-math.
			if (v != v) [[unlikely]]
				value = kvalue_nan;
		}
		LI_INLINE inline constexpr any(std::in_place_t, uint64_t value) : any_t{value} {}
		LI_INLINE inline constexpr any(any_t v) : any_t{v} {}
		LI_INLINE inline constexpr any(uint64_t v) = delete;

		// GC types.
		//
		LI_INLINE inline any(array* v) : any_t{mix_value(type_array, (uint64_t) v)} {}
		LI_INLINE inline any(table* v) : any_t{mix_value(type_table, (uint64_t) v)} {}
		LI_INLINE inline any(string* v) : any_t{mix_value(type_string, (uint64_t) v)} {}
		LI_INLINE inline any(vclass* v) : any_t{mix_value(type_class, (uint64_t) v)} {}
		LI_INLINE inline any(object* v) : any_t{mix_value(type_object, (uint64_t) v)} {}
		LI_INLINE inline any(function* v) : any_t{mix_value(type_function, (uint64_t) v)} {}
		LI_INLINE inline any(gc::header* v) : any_t{mix_value(gc::identify_value_type(v), (uint64_t) v)} {}

		// Constructs default value of the data type.
		//
		static any make_default(vm* L, li::type t);

		// Load/Store from data types.
		//
		static any load_from(const void* data, li::type t);
		void       store_at(void* data, li::type t) const;

		// Define comparison operators.
		//
		LI_INLINE inline constexpr bool operator==(const any& other) const { return equals(other); }
		LI_INLINE inline constexpr bool operator!=(const any& other) const { return !equals(other); }
		LI_INLINE inline friend constexpr bool operator==(const any& self, const any_t& other) { return self.equals(other); }
		LI_INLINE inline friend constexpr bool operator!=(const any& self, const any_t& other) { return !self.equals(other); }
		LI_INLINE inline friend constexpr bool operator==(const any_t& self, const any& other) { return self.equals(other); }
		LI_INLINE inline friend constexpr bool operator!=(const any_t& self, const any& other) { return !self.equals(other); }
	};
	static_assert(sizeof(any) == 8, "Invalid any size.");

	// Constants.
	//
	static constexpr any nil{};
	static constexpr any const_false{false};
	static constexpr any const_true{true};
	static constexpr any exception_marker{std::in_place, make_tag(type_exception)};

	// Fills the any[] with nils.
	//
	static void fill_nil(void* data, size_t count) {
		std::fill_n((uint64_t*) data, count, make_tag(type_nil));
	}
};