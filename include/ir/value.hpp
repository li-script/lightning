#pragma once
#include <vm/types.hpp>
#include <memory>
#include <string>
#include <util/format.hpp>
#include <util/typeinfo.hpp>
#include <vector>
#include <vm/bc.hpp>
#include <vm/string.hpp>
#include <vm/function.hpp>

namespace li::ir {
	using operation = bc::opcode;

	// Central value type.
	//
	struct value {
		// Reference counter.
		//
		mutable msize_t ref_counter = 1;
		mutable msize_t use_counter = 0;

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
		msize_t use_count() const { return use_counter; }
		msize_t ref_count() const { return ref_counter; }

		void add_ref(bool use) const {
			LI_ASSERT(ref_counter++ > 0);
			if (use)
				LI_ASSERT(use_counter++ >= 0);
		}
		void dec_ref(bool use) const {
			if (use)
				LI_ASSERT(--use_counter >= 0);
			msize_t new_ref = --ref_counter;
			LI_ASSERT(new_ref >= 0);
			if (!new_ref) {
				LI_ASSERT(use_counter == 0);
				delete (value*) this;
			}
		}

		// Recursive type check.
		//
		virtual bool rec_type_check(type x) { return false; }

		bool type_try_settle(type x, bool save = false) {
			if (vt == x || x == type::any)
				return true;
			if (vt != type::any)
				return false;
			type prev = std::exchange(vt, x);
			bool      r    = rec_type_check(x);
			vt             = (save && r) ? vt : prev;
			return r;
		}

		// Updates the instruction details such as return type/side effects.
		// Throws in debug mode if it's in an invalid state.
		//
		virtual void update() {}

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
		constexpr msize_t  use_count() const { return at->use_counter; }
		constexpr msize_t  ref_count() const { return at->ref_counter; }
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
			uint64_t          u;
			bool              i1;
			int32_t           i32;
			int64_t           i;
			operation         vmopr;
			value_type        vty;
			type         dty;
			double            n;
			gc::header*       gc;
			table*            tbl;
			array*            arr;
			vclass*           vcl;
			object*           obj;
			string*           str;
			function*         fn;
			basic_block*      bb;
			const nfunc_info* nfni;
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
		constexpr constant(object* v) : obj(v) { vt = type::obj; } // Resolve type ID.
		constexpr constant(vclass* v) : vcl(v) { vt = type::vcl; }
		constexpr constant(string* v) : str(v) { vt = type::str; }
		constexpr constant(function* v) : fn(v) { vt = type::fn; }
		constexpr constant(const nfunc_info* v) : nfni(v) { vt = type::nfni; }
		constexpr constant(basic_block* v) : bb(v) { vt = type::bb; }
		constexpr constant(operation v) : vmopr(v) { vt = type::vmopr; }
		constexpr constant(value_type v) : vty(v) { vt = type::vty; }
		constexpr constant(type v) : dty(v) { vt = type::dty; }
		constexpr constant(any a) {
			if (a.is_bool()) {
				i  = a.as_bool();
				vt = type::i1;
			} else if (a.is_num()) {
				n  = a.as_num();
				vt = type::f64;
			} else if (a.is_gc()) {
				vt = to_type(value_type(a.as_gc()->type_id));
				gc = a.as_gc();
			} else if (a.is_exc()) {
				vt = type::exc;
			} else {
				vt = type::nil;
			}
		}

		// Conversion to any.
		//
		any to_any() const {
			if (vt == type::i1) {
				return any(i1);
			} else if (is_integer_data(vt)) {
				return any(number(i));
			} else if (is_floating_point_data(vt)) {
				return any(n);
			} else if (vt == type::nil) {
				return any(nil);
			} else if (vt == type::exc) {
				return any(exception_marker);
			} else if (vt == type::tbl) {
				return any(tbl);
			} else if (vt == type::vcl) {
				return any(vcl);
			} else if (vt <= type::obj) {
				return any(obj);
			} else if (vt == type::arr) {
				return any(arr);
			} else if (vt == type::fn) {
				return any(fn);
			} else if (vt == type::str) {
				return any(str);
			} else {
				util::abort("cannot coerce %s to any", to_string().c_str());
			}
		}

		// Conversion to integer (not VM rules).
		//
		int64_t to_i64() const {
			if (vt == type::i1) {
				return i1;
			} else if (is_integer_data(vt)) {
				return i;
			} else if (is_floating_point_data(vt)) {
				return (int64_t) n;
			} else if (vt == type::nil) {
				return 0;
			} else if (vt == type::exc) {
				return 0;
			} else {
				return (uintptr_t) gc;
			}
		}
		bool to_bool() const {
			if (vt == type::i1 || is_integer_data(vt)) {
				return bool(i & 1);
			} else if (is_floating_point_data(vt)) {
				return true;
			} else if (vt == type::nil) {
				return false;
			} else if (vt == type::exc) {
				return false;
			} else {
				return gc != nullptr;
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