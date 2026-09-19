#include <vm/array.hpp>
#include <vm/function.hpp>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/shared.hpp>
#include <vm/state.hpp>
#include <vm/table.hpp>
#include <vm/traits.hpp>

namespace li {
	namespace {
		trait_set** mutable_trait_slot(any_t target) {
			if (target.is_tbl())
				return &target.as_tbl()->traits;
			if (target.is_vcl())
				return &target.as_vcl()->traits;
			return nullptr;
		}

		gc::header* direct_trait_owner(any_t target) {
			if (target.is_tbl())
				return target.as_tbl();
			if (target.is_vcl())
				return target.as_vcl();
			return nullptr;
		}

		const trait_set* direct_trait_set_unlocked(any_t target) {
			if (target.is_tbl())
				return target.as_tbl()->traits;
			if (target.is_vcl())
				return target.as_vcl()->traits;
			return nullptr;
		}

		bool descriptor_empty(const trait_set& value) {
			if (value.seal || value.freeze || value.hide)
				return false;
			for (function* method : value.methods) {
				if (method)
					return false;
			}
			return true;
		}

		trait_set* allocate_trait_set(vm* L, gc::header* owner) { return owner && owner->shared ? shared::allocate<trait_set>(L) : L->alloc<trait_set>(); }

		bool direct_flag(any_t target, trait which) {
			gc::header* owner = direct_trait_owner(target);
			if (!owner)
				return false;
			shared::recursive_guard guard(nullptr, owner);
			const trait_set*        traits = direct_trait_set_unlocked(target);
			if (!traits)
				return false;
			switch (which) {
				case trait::seal:
					return traits->seal;
				case trait::freeze:
					return traits->freeze;
				case trait::hide:
					return traits->hide;
				default:
					return false;
			}
		}

		function* lookup_trait(vm* L, any_t target, trait which, bool pin) {
			if (!is_trait_method(which))
				return nullptr;
			const size_t index = size_t(which);
			if (target.is_tbl()) {
				table*                  owner = target.as_tbl();
				shared::recursive_guard guard(L, owner);
				trait_set*              traits = owner->traits;
				function*               method = traits ? traits->methods[index] : nullptr;
				if (method && pin)
					rc::retain(method);
				return method;
			}

			vclass* current = nullptr;
			if (target.is_obj())
				current = target.as_obj()->cl;
			else if (target.is_vcl())
				current = target.as_vcl();
			for (; current; current = current->super) {
				shared::recursive_guard guard(L, current);
				if (current->traits && current->traits->methods[index]) {
					function* method = current->traits->methods[index];
					if (pin)
						rc::retain(method);
					return method;
				}
			}
			return nullptr;
		}

		bool trait_present(any_t target, trait which) {
			if (!is_trait_method(which))
				return false;
			const size_t index = size_t(which);
			if (target.is_tbl()) {
				table*                  owner = target.as_tbl();
				shared::recursive_guard guard(nullptr, owner);
				return owner->traits && owner->traits->methods[index];
			}

			vclass* current = nullptr;
			if (target.is_obj())
				current = target.as_obj()->cl;
			else if (target.is_vcl())
				current = target.as_vcl();
			for (; current; current = current->super) {
				shared::recursive_guard guard(nullptr, current);
				if (current->traits && current->traits->methods[index])
					return true;
			}
			return false;
		}

		any_t get_trait_impl(vm* L, any_t target, trait which) {
			if (!target.is_tbl() && !target.is_vcl() && !target.is_obj())
				return L->error("traits can only be inspected on a table, class, or object");
			if (is_trait_flag(which))
				return L->ok(target.is_obj() ? trait_flag_enabled(target, which) : direct_flag(target, which));
			if (trait_flag_enabled(target, trait::hide))
				return L->ok();
			if (function* method = pin_trait(L, target, which)) {
				any result = L->take(any(method));
				if (result.is_exc())
					rc::release(L, method);
				return result;
			}
			return L->ok();
		}

		any_t set_trait_impl(vm* L, any_t target, trait which, any_t value) {
			trait_set** slot  = mutable_trait_slot(target);
			gc::header* owner = direct_trait_owner(target);
			if (!slot || !owner)
				return L->error("traits can only be installed on a table or class");

			const bool is_flag = is_trait_flag(which);
			const bool enabled = is_flag && value.coerce_bool();
			if (!is_flag && value != nil && !value.is_fn())
				return L->error("trait '%.*s' requires a function or nil", int(trait_names[size_t(which)].size()), trait_names[size_t(which)].data());

			shared::prepared_value prepared{};
			if (!is_flag) {
				prepared = shared::prepare_store(L, owner, value);
				if (!prepared.ok)
					return exception_marker;
				if (prepared.value != nil && !prepared.value.is_fn()) {
					shared::finish_store(L, prepared);
					return L->error(
						 "trait '%.*s' requires a transferable function or nil", int(trait_names[size_t(which)].size()), trait_names[size_t(which)].data());
				}
			}

			trait_set* candidate     = (is_flag ? enabled : prepared.value != nil) ? allocate_trait_set(L, owner) : nullptr;
			bool       sealed        = false;
			bool       retain_failed = false;
			{
				shared::recursive_guard guard(L, owner);
				if (*slot && (*slot)->seal) {
					sealed = true;
				} else if (is_flag) {
					if (*slot || enabled) {
						if (!*slot) {
							*slot     = candidate;
							candidate = nullptr;
						}
						switch (which) {
							case trait::seal:
								(*slot)->seal = enabled;
								break;
							case trait::freeze:
								(*slot)->freeze = enabled;
								break;
							case trait::hide:
								(*slot)->hide = enabled;
								break;
							default:
								assume_unreachable();
						}
					}
				} else if (*slot || prepared.value != nil) {
					if (!*slot) {
						*slot     = candidate;
						candidate = nullptr;
					}
					function*  stored = prepared.value == nil ? nullptr : prepared.value.as_fn();
					function*& entry  = (*slot)->methods[size_t(which)];
					if (entry != stored) {
						if (!rc::try_retain(L, stored)) {
							retain_failed = true;
						} else {
							function* previous = entry;
							entry              = stored;
							rc::release(L, previous);
						}
					}
				}

				if (!sealed && !retain_failed && *slot && descriptor_empty(**slot))
					destroy_trait_set(L, *slot);
			}

			if (candidate)
				rc::release(L, candidate);
			shared::finish_store(L, prepared);
			if (sealed)
				return L->error("modifying sealed traits");
			if (retain_failed)
				return exception_marker;
			return L->ok();
		}

		any_t invoke_finalizer(vm* L, function* method, any self) {
			// The object already carries the destroying sentinel. Put self in the
			// frame as a genuinely borrowed value so entering the finalizer does
			// not retain it; any attempted escape still goes through rc::retain and
			// is rejected as resurrection.
			if (!L->vm_stack_headroom_available(4)) [[unlikely]]
				return exception_marker;
			any* reset = L->stack_top;
			L->alloc_stack(1);  // Keeps the zero-argument frame address in-bounds.
			any* self_slot = L->alloc_stack(1);
			*self_slot     = self;
			L->push_stack(any(method));
			call_frame frame{
				 .caller_pc = msize_t(L->last_vm_caller.caller_pc | FRAME_C_FLAG),
				 .stack_pos = L->last_vm_caller.stack_pos,
			};
			L->push_stack(any_t{li::bit_cast<uint64_t>(frame)});
			any result = vm_invoke(L, self_slot - 1, 0);
			*self_slot = nil;
			L->truncate_stack(reset);
			return result;
		}
	}

	trait_set* clone_trait_set(vm* L, const trait_set* source) {
		if (!source)
			return nullptr;
		trait_set* result = allocate_trait_set(L, nullptr);
		result->seal      = source->seal;
		result->freeze    = source->freeze;
		result->hide      = source->hide;
		for (size_t i = 0; i != num_trait_methods; ++i) {
			result->methods[i] = source->methods[i];
			rc::retain(result->methods[i]);
		}
		return result;
	}

	void destroy_trait_set(vm* L, trait_set*& value) {
		trait_set* current = value;
		value              = nullptr;
		if (!current)
			return;
		for (function*& method : current->methods) {
			function* owned = method;
			method          = nullptr;
			rc::release(L, owned);
		}
		rc::release(L, current);
	}

	function* resolve_trait(any_t target, trait which) { return lookup_trait(nullptr, target, which, false); }

	function* pin_trait(vm* L, any_t target, trait which) { return lookup_trait(L, target, which, true); }

	bool has_trait(any_t target, trait which) { return trait_present(target, which); }

	bool trait_flag_enabled(any_t target, trait which) {
		if (!is_trait_flag(which))
			return false;
		if (target.is_tbl())
			return direct_flag(target, which);

		vclass* current = nullptr;
		if (target.is_obj())
			current = target.as_obj()->cl;
		else if (target.is_vcl())
			current = target.as_vcl();
		for (; current; current = current->super) {
			if (direct_flag(any(current), which))
				return true;
		}
		return false;
	}

	trait resolve_trait_name(std::string_view name) {
		for (size_t i = 0; i != trait_names.size(); ++i) {
			if (trait_names[i] == name)
				return static_cast<trait>(i);
		}
		return trait::none;
	}

	any_t get_trait(vm* L, any_t target, trait which) {
		if (which == trait::none || size_t(which) >= num_traits)
			return L->error("invalid trait");
		return get_trait_impl(L, target, which);
	}

	any_t set_trait(vm* L, any_t target, trait which, any_t value) {
		if (which == trait::none || size_t(which) >= num_traits)
			return L->error("invalid trait");
		return set_trait_impl(L, target, which, value);
	}

	any_t invoke_trait(vm* L, trait which, any_t self, std::span<const any> args) {
		function* method = pin_trait(L, self, which);
		if (!method)
			return L->error("value has no '%.*s' trait", int(trait_names[size_t(which)].size()), trait_names[size_t(which)].data());
		if (!L->vm_stack_headroom_available(args.size() + FRAME_SIZE)) [[unlikely]] {
			rc::release(L, method);
			return L->vm_stack_exhausted();
		}
		for (size_t i = args.size(); i != 0; --i)
			L->push_stack(args[i - 1]);
		any result = L->call(slot_t(args.size()), any(method), self);
		rc::release(L, method);
		return result;
	}

	void run_trait_finalizer(vm* L, gc::header* value) {
		if (!value)
			return;
		any self;
		if (value->type_id == type_table)
			self = any(reinterpret_cast<table*>(value));
		else if (gc::identify_value_type(value) == type_object) {
			auto* instance = reinterpret_cast<object*>(value);
			if (!instance->finalizable)
				return;
			self = any(instance);
		} else
			return;

		function* method = pin_trait(L, self, trait::del);
		if (!method)
			return;

		any    previous_exception = L->last_ex;
		array* previous_trace     = L->last_exception_trace;
		rc::retain(previous_exception);
		rc::retain(previous_trace);
		any result = invoke_finalizer(L, method, self);
		rc::release(L, method);
		if (!result.is_exc())
			rc::release(L, result);
		rc::replace_adopt(L, L->last_ex, previous_exception);
		array* discarded_trace  = L->last_exception_trace;
		L->last_exception_trace = previous_trace;
		rc::release(L, discarded_trace);
	}
};
