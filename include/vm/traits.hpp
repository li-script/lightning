#pragma once
#include <util/common.hpp>
#include <util/format.hpp>
#include <vm/gc.hpp>
#include <lang/types.hpp>

namespace li {
	// Table traits.
	// All non-flag entries are guaranteed to be function* except of get, which can be a table.
	//
#define LIGHTNING_ENUM_TRAIT(_, __) \
	/* Iterable traits. */				\
	_(get) _(set) _(len)					\
	/* Arithmetic traits. */			\
	_(neg) _(add) _(sub)					\
	_(mul) _(div) _(mod)					\
	_(pow)									\
	/* Comparison traits. */			\
	_(lt) _(le) _(eq)						\
	/* Misc. traits. */					\
	_(call) _(str) _(gc)					\
	/* Flag traits. */					\
	__(seal) __(freeze) __(hide)
	
	enum class trait : uint8_t {
#define TRAIT_WRITE(a)  a,
#define TRAIT_RET(name) return ( uint8_t ) trait::name;
		LIGHTNING_ENUM_TRAIT(TRAIT_WRITE, LI_NOOP)
		LIGHTNING_ENUM_TRAIT(LI_NOOP, TRAIT_WRITE)
		pseudo_max,
		max = []() { LIGHTNING_ENUM_TRAIT(LI_NOOP, TRAIT_RET); }(),
#undef TRAIT_RET
#undef TRAIT_WRITE
		none = 0xFF
	};
	static constexpr std::string_view trait_names[] = {
#define TRAIT_NAME(name)  #name,
		 LIGHTNING_ENUM_TRAIT(TRAIT_NAME, LI_NOOP)
		 LIGHTNING_ENUM_TRAIT(LI_NOOP, TRAIT_NAME)
#undef TRAIT_NAME
	};
	static constexpr msize_t num_traits = (msize_t) trait::max;  // Does not include flags.

	// Compressed trait pointer.
	//
	struct trait_pointer {
		uintptr_t t : 1 = 0;  // Table bit, switches to table, must be handled specifically if used.
#if LI_32
		intptr_t pointer : 31 = 0;
#else
		intptr_t pointer : 63 = 0;
#endif

		// Default construction and construction by any.
		//
		constexpr trait_pointer() {}
		trait_pointer(any a) {
			if (a.is_fn()) {
				t = false;
			} else {
				t = true;
				LI_ASSERT(a.is_tbl());
			}
			pointer = intptr_t(a.as_gc());
		}

		// Replicate any interface.
		//
		constexpr bool is_tbl() const { return t; }
		gc::header*    as_gc() const { return (gc::header*) uintptr_t(intptr_t(pointer)); }
		function*      as_fn() const { return (function*) as_gc(); }
		table*         as_tbl() const { return (table*) as_gc(); }
		any            as_any() const { return t ? any(as_tbl()) : any(as_fn()); }
	};
	struct trait_table : gc::leaf<trait_table> {
		trait_pointer list[num_traits] = {};
	};

	// Final tag.
	//
	template<typename T = void, value_type V = type_nil>
	struct traitful_node : gc::node<T, V> {
		trait_table* traits                  = nullptr;  // Table of traits.
		uint32_t     trait_freeze : 1        = 0;        // Allows constant optimizations and errors on value set.
		uint32_t     trait_seal : 1          = 0;        // Allows constant optimizations and errors on trait set.
		uint32_t     trait_hide : 1          = 0;        // Hides the metatable from getter.
		uint32_t     trait_mask : num_traits = 0;        // Mask of non-null traits.

		template<trait Ti>
		LI_INLINE bool has_trait() const {
			return trait_mask & (1u << uint32_t(Ti));
		}
		template<trait Ti>
		LI_INLINE any get_trait() const {
			return get_trait(Ti);
		}

		// Getter.
		//
		LI_INLINE any get_trait(trait t) const {
			// Pseudo traits.
			//
			if (t >= trait::max) {
				if (t == trait::seal) {
					return (bool) trait_seal;
				} else if (t == trait::freeze) {
					return (bool) trait_freeze;
				} else if (t == trait::hide) {
					return (bool) trait_hide;
				}
			}

			// Fields.
			//
			if (trait_mask & (1u << msize_t(t))) {
				return traits->list[msize_t(t)].as_any();
			} else {
				return nil;
			}
		}

		// Setter, returns non-null string on error.
		//
		const char* set_trait(vm* L, trait t, any v) {
			if (trait_seal) {
				return "modifying sealed traits.";
			}

			// Pseudo traits.
			//
			if (t >= trait::max) {
				if (t == trait::seal) {
					trait_seal = v.coerce_bool();
					return nullptr;
				} else if (t == trait::freeze) {
					trait_freeze = v.coerce_bool();
					return nullptr;
				} else if (t == trait::hide) {
					trait_hide = v.coerce_bool();
					return nullptr;
				}
			}

			// Set value.
			//
			if (v != nil) {
				// Type check.
				//
				if (!v.is_fn()) {
					if (v.is_tbl() && t != trait::get) {
						return "only get trait can be a table";
					}
				}

				if (!traits) {
					traits = L->alloc<trait_table>();
				}

				trait_mask |= 1u << msize_t(t);
				traits->list[msize_t(t)] = v;
			} else {
				traits->list[msize_t(t)] = {};
				trait_mask &= ~(1u << msize_t(t));

				if (!trait_mask && traits) {
					traits = nullptr;
				}
			}
			return nullptr;
		}

		// GC traverse implementation.
		//
		void trait_traverse(gc::stage_context s) {
			if (auto* tl = traits) {
				tl->gc_tick(s);
				for (msize_t i = 0; i != num_traits; i++) {
					if (trait_mask & (1 << i)) {
						tl->list[i].as_gc()->gc_tick(s);
					}
				}
			}
		}

		// GC callback.
		//
		void gc_destroy(vm* L) {
			if (trait_mask & (1u << msize_t(trait::gc))) {
				any self;
				if constexpr (std::is_void_v<T>) {
					self = any_t{mix_value(((gc::header*) this)->gc_type, (uint64_t) this)};
				} else {
					self = any((T*) this);
				}
				L->call(0, traits->list[msize_t(trait::gc)].as_any(), self);
				L->pop_stack();
			}
		}
	};
};