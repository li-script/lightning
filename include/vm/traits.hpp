#pragma once
#include <array>
#include <span>
#include <string_view>
#include <vm/gc.hpp>

namespace li {
	struct function;
	struct vm;

	// Methods and flags shared by tables and classes. `del` is the canonical
	// spelling of the deterministic finalizer formerly called `gc`.
	enum class trait : uint8_t {
		at,
		set,
		len,
		neg,
		add,
		sub,
		mul,
		div,
		mod,
		pow,
		lt,
		eq,
		call,
		str,
		del,
		next,
		seal,
		freeze,
		hide,
		count,
		none = 0xFF,
	};

	inline constexpr size_t                                   num_trait_methods = static_cast<size_t>(trait::seal);
	inline constexpr size_t                                   num_traits        = static_cast<size_t>(trait::count);
	inline constexpr std::array<std::string_view, num_traits> trait_names       = {
		 "at",
		 "set",
		 "len",
		 "neg",
		 "add",
		 "sub",
		 "mul",
		 "div",
		 "mod",
		 "pow",
		 "lt",
		 "eq",
		 "call",
		 "str",
		 "del",
		 "next",
		 "seal",
		 "freeze",
		 "hide",
	};

	constexpr bool is_trait_method(trait value) { return static_cast<size_t>(value) < num_trait_methods; }
	constexpr bool is_trait_flag(trait value) { return value == trait::seal || value == trait::freeze || value == trait::hide; }

	// Allocated only after the first method or true flag is installed. Every
	// non-null method is an owning reference.
	struct trait_set : gc::leaf<trait_set> {
		std::array<function*, num_trait_methods> methods{};
		uint8_t                                  seal : 1     = false;
		uint8_t                                  freeze : 1   = false;
		uint8_t                                  hide : 1     = false;
		uint8_t                                  reserved : 5 = 0;
	};

	// Descriptor ownership helpers. Cloning makes an independent descriptor and
	// retains every installed method. Destruction nulls the owner's pointer.
	trait_set* clone_trait_set(vm* L, const trait_set* source);
	void       destroy_trait_set(vm* L, trait_set*& value);

	// Looks up an installed method. Object/class lookup walks the superclass
	// chain; table lookup is direct. resolve_trait returns a borrowed function
	// and is only suitable when the owner cannot mutate concurrently. pin_trait
	// returns an owned function retained while the shared owner lock is held.
	function* resolve_trait(any_t target, trait which);
	function* pin_trait(vm* L, any_t target, trait which);
	bool      has_trait(any_t target, trait which);
	bool      trait_flag_enabled(any_t target, trait which);

	// Invokes an installed method with borrowed self/arguments and transfers an
	// owned result to the caller. A missing method is a language error.
	any_t invoke_trait(vm* L, trait which, any_t self, std::span<const any> args = {});

	// Dynamic reflection is deliberately separate from source declarations.
	// Invalid names resolve to none; get/set reject none and non-trait-bearing
	// targets. Returned values are owned.
	trait resolve_trait_name(std::string_view name);
	any_t get_trait(vm* L, any_t target, trait which);
	any_t set_trait(vm* L, any_t target, trait which, any_t value);

	// Invoked centrally by RC after the weak target is invalidated and before
	// outgoing references are released. Only table and object values finalize.
	void run_trait_finalizer(vm* L, gc::header* value);
};
