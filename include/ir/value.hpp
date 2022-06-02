#pragma once
#include <vector>
#include <string>
#include <memory>
#include <util/format.hpp>
#include <util/typeinfo.hpp>
#include <lang/types.hpp>
#include <vm/bc.hpp>
#include <vm/string.hpp>

namespace li::ir {
	struct insn;
	struct basic_block;
	struct procedure;

	// Value types.
	//
	enum class type : uint8_t {
		none,

		// Wrapped type.
		//
		unk,

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

		// VM types.
		//
		opq,
		tbl, // same order as gc types
		udt,
		arr,
		vfn,
		nfn,
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
	using operation = bc::opcode;

	// Central value type.
	//
	struct value
	{
		// Type id of this.
		//
		util::type_id ti;

		// Value type.
		//
		type vt = type::none;

		// Constructed by type id.
		//
		constexpr value(util::type_id ti) : ti(ti) {}

		// Dynamic cast.
		//
		template<typename T>
		bool is() const {
			return util::check_type_id_no_cv<T>(ti);
		}
		template<typename T>
		T* get() {
			return (T*) (is<T>() ? (void*) this : nullptr);
		}
		template<typename T>
		const T* get() const {
			return (const T*) (is<T>() ? (void*) this : nullptr);
		}

		// Value type check.
		//
		bool is(type t) const { return vt == t; }

		// Printer.
		//
		virtual std::string to_string(bool expand = false) const = 0;

		// Virtual destructor.
		//
		virtual ~value() = default;
	};

	// Reference to value type.
	//
	using value_ref = std::shared_ptr<value>;

	// Default constructed tag for automatically setting ti.
	//
	template<typename T>
	struct value_tag : value {
		constexpr value_tag() : value(util::type_id_v<T>) {}
	};

	// Forward decl for bb name printer.
	//
	static std::string to_string(basic_block* bb);

	// Constant type.
	//
	struct constant final : value_tag<constant> {
		union {
			uint64_t     u;
			int64_t      i;
			operation    vmopr;
			trait        vmtrait;
			value_type   vmtype;
			type         irtype;
			double       n;
			gc::header*  gc;
			table*       tbl;
			array*       arr;
			userdata*    udt;
			string*      str;
			function*    vfn;
			nfunction*   nfn;
			opaque       opq;
			basic_block* bb;
		};

		constexpr constant() { vt = type::none; }
		constexpr constant(bool v) : i(v ? 1 : 0) { vt = type::i1; }
		constexpr constant(int8_t v) : i(v) { vt = type::i8; }
		constexpr constant(int16_t v) : i(v) { vt = type::i16; }
		constexpr constant(int32_t v) : i(v) { vt = type::i32; }
		constexpr constant(int64_t v) : i(v) { vt = type::i64; }
		constexpr constant(float v) : n(v) { vt = type::f32; }
		constexpr constant(double v) : n(v) { vt = type::f64; }
		constexpr constant(table* v) : tbl(v) { vt = type::tbl; }
		constexpr constant(array* v) : arr(v) { vt = type::arr; }
		constexpr constant(userdata* v) : udt(v) { vt = type::udt; }
		constexpr constant(string* v) : str(v) { vt = type::str; }
		constexpr constant(function* v) : vfn(v) { vt = type::vfn; }
		constexpr constant(nfunction* v) : nfn(v) { vt = type::nfn; }
		constexpr constant(opaque v) : opq(v) { vt = type::opq; }
		constexpr constant(basic_block* v) : bb(v) { vt = type::bb; }

		constexpr constant(operation v) : vmopr(v) { vt = type::vmopr; }
		constexpr constant(trait v) : vmtrait(v) { vt = type::vmtrait; }
		constexpr constant(value_type v) : vmtype(v) { vt = type::vmtype; }
		constexpr constant(type v) : irtype(v) { vt = type::irtype; }

		constant(any a) {
			if (a.is_gc()) {
				auto t = a.as_gc()->gc_type;
				LI_ASSERT(type_table <= t && t <= type_string);
				vt = type(uint8_t(t) + uint8_t(type::tbl) - type_table);
			} else if (a.is_bool()) {
				i  = a.as_bool();
				vt = type::i1;
			} else if (a.is_opq()) {
				opq = a.as_opq();
				vt  = type::opq;
			} else if (a.is_num()) {
				n  = a.as_num();
				vt = type::f64;
			}
		}

		// Implement printer.
		//
		std::string to_string(bool = false) const override;
	};
};