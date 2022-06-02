#pragma once
#include <lang/types.hpp>
#include <memory>
#include <string>
#include <util/format.hpp>
#include <util/typeinfo.hpp>
#include <vector>
#include <vm/bc.hpp>
#include <vm/string.hpp>

namespace li::ir {
	struct insn;
	struct constant;
	struct basic_block;
	struct procedure;

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

		// Wrapped type.
		//
		unk,

		// VM types.
		//
		nil,
		opq,
		tbl,  // same order as gc types
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
	struct value {
		// Reference counter.
		//
		mutable uint32_t ref_counter = 1;
		mutable uint32_t use_counter = 0;

		// Type id of this.
		//
		util::type_id ti;

		// Value type.
		//
		type vt = type::none;

		// Constructed by type id.
		//
		constexpr value(util::type_id ti) : ti(ti) {}

		// Copy, ignores ref-counter.
		//
		constexpr value(const value& other) : ti(other.ti), vt(other.vt) {}
		constexpr value& operator=(const value& other) {
			ti = other.ti;
			vt = other.vt;
			return *this;
		}

		// Dynamic cast.
		//
		template<typename T>
		bool is() const {
			if constexpr (std::is_base_of_v<insn, T> && !std::is_same_v<insn, T>) {
				return util::test_type_id_no_cv<insn>(ti) && ((T*) this)->opc == T::Opcode;
			}
			return util::test_type_id_no_cv<T>(ti);
		}
		template<typename T>
		T* as() {
			LI_ASSERT(is<T>());
			return (T*) this;
		}
		template<typename T>
		const T* as() const {
			LI_ASSERT(is<T>());
			return (const T*) this;
		}

		// Value type check.
		//
		bool is(type t) const { return vt == t; }

		// Reference helpers.
		//
		uint32_t use_count() const { return use_counter; }
		uint32_t ref_count() const { return ref_counter; }

		void add_ref(bool use) const {
			LI_ASSERT(ref_counter++ > 0);
			if (use)
				LI_ASSERT(use_counter++ >= 0);
		}
		void dec_ref(bool use) const {
			if (use)
				LI_ASSERT(--use_counter >= 0);
			uint32_t new_ref = --ref_counter;
			LI_ASSERT(new_ref >= 0);
			if (!new_ref) {
				LI_ASSERT(use_counter == 0);
				delete (value*) this;
			}
		}

		virtual void add_user(insn* a) const {}
		virtual void del_user(insn* a) const {}

		// Printer.
		//
		virtual std::string to_string(bool expand = false) const = 0;

		// Virtual destructor.
		//
		virtual ~value() = default;
	};

	// Reference counter for value types.
	//
	template<typename T, bool Use>
	struct LI_TRIVIAL_ABI basic_value_ref {
		T* at = nullptr;

		// Raw construction.
		//
		constexpr basic_value_ref() = default;
		constexpr basic_value_ref(std::nullptr_t) {}
		constexpr basic_value_ref(std::in_place_t, T* v) : at(v) {}
		constexpr basic_value_ref(const basic_value_ref& o) { reset(o.at); }
		constexpr basic_value_ref& operator=(const basic_value_ref& o) {
			reset(o.at);
			return *this;
		}
		constexpr basic_value_ref(basic_value_ref&& o) noexcept : at(std::exchange(o.at, nullptr)) {}
		constexpr basic_value_ref& operator=(basic_value_ref&& o) noexcept {
			std::swap(at, o.at);
			return *this;
		}

		// Copy and move.
		//
		template<typename Ty, bool UseY>
		constexpr basic_value_ref(const basic_value_ref<Ty, UseY>& o) {
			reset((T*) o.at);
		}
		template<typename Ty, bool UseY>
		constexpr basic_value_ref& operator=(const basic_value_ref<Ty, UseY>& o) {
			reset((T*) o.at);
			return *this;
		}
		template<typename Ty, bool UseY>
		constexpr basic_value_ref(basic_value_ref<Ty, UseY>&& o) noexcept {
			if constexpr (UseY != Use) {
				if (o) {
					if constexpr (Use)
						LI_ASSERT(o.at->use_counter++ >= 0);
					else
						LI_ASSERT(--o.at->use_counter >= 0);
				}
			}
			at = (T*) o.release();
		}
		template<typename Ty, bool UseY>
		constexpr basic_value_ref& operator=(basic_value_ref<Ty, UseY>&& o) noexcept {
			if constexpr (UseY != Use) {
				if (o) {
					if constexpr (Use)
						LI_ASSERT(o.at->use_counter++ >= 0);
					else
						LI_ASSERT(--o.at->use_counter >= 0);
				}
			}
			if (T* prev = std::exchange(at, (T*) o.release()))
				prev->dec_ref(Use);
			return *this;
		}

		// Observers
		//
		constexpr T*       get() const { return at; }
		constexpr T*       operator->() const { return at; }
		constexpr          operator T*() const { return at; }
		constexpr T&       operator*() const { return *at; }
		constexpr uint32_t use_count() const { return at->use_counter; }
		constexpr uint32_t ref_count() const { return at->ref_counter; }
		constexpr explicit operator bool() const { return at != nullptr; }

		// Reset and release.
		//
		constexpr void reset(T* o = nullptr) {
			if (o != at) {
				if (o)
					o->add_ref(Use);
				if (at)
					at->dec_ref(Use);
				at = o;
			}
		}
		constexpr T* release() { return std::exchange(at, nullptr); }

		// Reset on destruction.
		//
		constexpr ~basic_value_ref() { reset(); }
	};
	template<typename T = value>
	using ref = basic_value_ref<T, false>;
	template<typename T = value>
	using use = basic_value_ref<T, true>;

	template<typename T, typename... Tx>
	inline static ref<T> make_value(Tx&&... args) {
		return ref<T>(std::in_place, new T(std::forward<Tx>(args)...));
	}
	template<typename T>
	inline static ref<T> make_ref(T* o) {
		o->add_ref(false);
		return ref<T>(std::in_place, o);
	}
	template<typename T>
	inline static use<T> make_use(T* o) {
		o->add_ref(true);
		return use<T>(std::in_place, o);
	}

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
			bool         i1;
			int32_t      i32;
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

		// Default construction and copy.
		//
		constexpr constant() : i(0) { vt = type::none; }
		constexpr constant(const constant& o) : i(o.i) { vt = o.vt; }
		constexpr constant& operator=(const constant& o) {
			i  = o.i;
			vt = o.vt;
			return *this;
		}

		// Construction by immediate.
		//
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
		constexpr constant(any a) {
			if (a.is_bool()) {
				i  = a.as_bool();
				vt = type::i1;
			} else if (a.is_opq()) {
				opq = a.as_opq();
				vt  = type::opq;
			} else if (a.is_num()) {
				n  = a.as_num();
				vt = type::f64;
			} else if (a.is_gc()) {
				auto t = a.as_gc()->gc_type;
				LI_ASSERT(type_table <= t && t <= type_string);
				vt = type(uint8_t(t) + uint8_t(type::tbl) - type_table);
				gc = a.as_gc();
			} else {
				vt = type::nil;
			}
		}

		// Equality comparison.
		//
		constexpr bool operator==(const constant& other) const { return i == other.i && vt == other.vt; }

		// Implement printer.
		//
		std::string to_string(bool = false) const override;
	};
};