#pragma once
#include <util/common.hpp>
#include <util/format.hpp>
#include <vm/gc.hpp>
#include <lang/types.hpp>

namespace li {
	// Table traits.
	// All entries are guaranteed to be either <function|nfunction> except of get, which can be a table.
	//
	enum class trait : uint8_t {
		// Iterable traits.
		//
		get,
		set,
		len,
		in,

		// Arithmetic traits.
		//
		neg,
		add,
		sub,
		mul,
		div,
		mod,
		pow,

		// Comparison traits.
		//
		lt,
		le,
		eq,

		// Misc.
		//
		call, // Invocation.
		str,  // Type coercion to string.
		gc,   // Invoked on destruction.

		// Pseudo-indices.
		max,
		seal = max,
		hide
	};
	static constexpr uint32_t num_traits = (uint32_t) trait::max;

	// Compressed trait pointer.
	//
	struct trait_pointer {
		uintptr_t n : 1 = 0;  // Native bit, switches to nfunction.
		uintptr_t t : 1 = 0;  // Table bit, switches to table, must be handled specifically if used, is_nfn / is_vfn ignores it.
#if LI_32
		uintptr_t pointer : 30 = 0;
#else
		uintptr_t pointer : 62 = 0;
#endif

		// Default construction and construction by any.
		//
		constexpr trait_pointer() {}
		trait_pointer(any a) {
			if (a.is_nfn()) {
				n = true;
			} else if (a.is_vfn()) {
			} else {
				t = true;
				LI_ASSERT(a.is_tbl());
			}
			pointer = uint64_t(a.as_gc()) >> 2;
		}

		// Replicate any interface.
		//
		constexpr bool is_nfn() const { return n; }
		constexpr bool is_vfn() const { return !n; }
		constexpr bool is_tbl() const { return t; }
		gc::header*    as_gc() const { return (gc::header*) (pointer << 2); }
		nfunction*     as_nfn() const { return (nfunction*) as_gc(); }
		function*      as_vfn() const { return (function*) as_gc(); }
		table*         as_tbl() const { return (table*) as_gc(); }
		any            as_any() const { return t ? any(as_tbl()) : (n ? any(as_nfn()) : any(as_vfn())); }
	};

	// Final table.
	//
	struct table_traits : gc::leaf<table_traits> {
		uint32_t      seal : 1            = 0;   // Allows constant optimizations and errors on set.
		uint32_t      hide : 1            = 0;   // Hides the metatable from getter.
		uint32_t      mask : num_traits   = 0;   // Mask of non-null traits.
		trait_pointer traits[num_traits]  = {};  // List of traits.

		// Getter.
		//
		std::pair<const char*, any> get(trait t) const {
			// Pseudo traits.
			//
			if (t >= trait::max) {
				if (t == trait::seal) {
					return {nullptr, any(bool(seal))};
				} else if (t == trait::hide) {
					return {nullptr, any(bool(hide))};
				}
				return {"invalid trait id", any()};
			}

			// Fields.
			//
			if (mask & (1u << uint32_t(t))) {
				return {nullptr, traits[uint32_t(t)].as_any()};
			} else {
				return {nullptr, none};
			}
		}

		// Setter, returns non-null string on error.
		//
		const char* set(trait t, any v) {
			// Pseudo traits.
			//
			if (t >= trait::max) {
				if (t == trait::seal) {
					seal = v.as_bool();
					return nullptr;
				} else if (t == trait::hide) {
					hide = v.as_bool();
					return nullptr;
				}
				return "invalid trait id";
			}

			// Type check.
			//
			if (!v.is_vfn() && !v.is_nfn()) {
				if (v.is_tbl() && t != trait::get) {
					return "only get trait can be a table";
				}
			}

			// Set value.
			//
			if (v != none) {
				mask |= 1u << uint32_t(t);
				traits[uint32_t(t)] = v;
			} else {
				mask &= ~(1u << uint32_t(t));
			}
			return nullptr;
		}

		// GC traverse implementation.
		//
		void gc_traverse(gc::stage_context s) override {
			for (uint32_t i = 0; i != num_traits; i++) {
				if (mask & (1 << i)) {
					traits[i].as_gc()->gc_tick(s);
				}
			}
		}
	};
};