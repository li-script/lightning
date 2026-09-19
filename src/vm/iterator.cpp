#include <array>
#include <cmath>
#include <new>
#include <util/typeinfo.hpp>
#include <util/user.hpp>
#include <vm/array.hpp>
#include <vm/coroutine.hpp>
#include <vm/iterator.hpp>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/shared.hpp>
#include <vm/state.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <vm/traits.hpp>
#include <vm/typed_array.hpp>

namespace li {
	namespace {
		enum class iterator_kind : uint8_t {
			collection,
			forward,
			range,
			map,
			filter,
		};

		struct iterator_state {
			vm*           owner;
			iterator_kind kind;
			any           target    = nil;
			function*     callback  = nullptr;
			uint64_t      version   = 0;
			msize_t       position  = 0;
			number        current   = 0;
			number        end       = 0;
			number        step      = 1;
			bool          open_end  = false;
			bool          inclusive = false;
			bool          exhausted = false;

			iterator_state(vm* L, iterator_kind kind, any_t target = nil, function* callback = nullptr)
				 : owner(L), kind(kind), target(target), callback(callback) {
				rc::retain(target);
				rc::retain(callback);
				if (kind == iterator_kind::collection) {
					shared::recursive_guard guard(L, target.is_gc() ? target.as_gc() : nullptr);
					if (target.is_arr())
						version = target.as_arr()->mutation_version;
					else if (target.is_tarr())
						version = target.as_tarr()->mutation_version;
					else if (target.is_tbl())
						version = target.as_tbl()->mutation_version;
				}
			}

			~iterator_state() {
				rc::release(owner, target);
				rc::release(owner, callback);
				target   = nil;
				callback = nullptr;
			}
		};

		struct internal_result {
			any         key = nil;
			next_result next{};
		};

		iterator_state* state_from_object(object* value) noexcept {
			const auto base    = reinterpret_cast<std::uintptr_t>(value->context);
			const auto aligned = (base + alignof(iterator_state) - 1) & ~(std::uintptr_t(alignof(iterator_state)) - 1);
			return reinterpret_cast<iterator_state*>(aligned);
		}

		void iterator_gc_hook(object* instance) { std::destroy_at(state_from_object(instance)); }

		iterator_state* state_from_value(any_t value) noexcept {
			if (!value.is_obj())
				return nullptr;
			object* instance = value.as_obj();
			if (instance->gc_hook != &iterator_gc_hook || !instance->cl || instance->cl->cxx_tid != util::type_id_v<iterator_state>)
				return nullptr;
			return state_from_object(instance);
		}

		object* create_state_object(vm* L, vclass* cl, iterator_kind kind, any_t target = nil, function* callback = nullptr) {
			if (!cl) {
				L->error("iterator library is not registered");
				return nullptr;
			}
			object* instance = object::create(L, cl);
			std::construct_at(state_from_object(instance), L, kind, target, callback);
			instance->gc_hook = &iterator_gc_hook;
			return instance;
		}

		bool collection_is_unchanged_unlocked(const iterator_state& state) {
			if (state.target.is_arr())
				return state.target.as_arr()->mutation_version == state.version;
			if (state.target.is_tarr())
				return state.target.as_tarr()->mutation_version == state.version;
			if (state.target.is_tbl())
				return state.target.as_tbl()->mutation_version == state.version;
			return true;
		}

		bool collection_is_unchanged(vm* L, const iterator_state& state) {
			shared::recursive_guard guard(L, state.target.is_gc() ? state.target.as_gc() : nullptr);
			return collection_is_unchanged_unlocked(state);
		}

		bool cursor_is_valid(vm* L, iterator_state& state) {
			if (state.exhausted)
				return true;
			if (state.kind == iterator_kind::collection) {
				if (collection_is_unchanged(L, state))
					return true;
				L->error("collection mutated during iteration");
				return false;
			}
			if (state.kind == iterator_kind::map || state.kind == iterator_kind::filter) {
				iterator_state* source = state_from_value(state.target);
				return source && cursor_is_valid(L, *source);
			}
			if (state.kind == iterator_kind::forward) {
				if (iterator_state* source = state_from_value(state.target))
					return cursor_is_valid(L, *source);
			}
			return true;
		}

		bool retain_pair(vm* L, any_t key, any_t value, any& owned_key, any& owned_value) {
			if (!rc::try_retain(L, key))
				return false;
			if (!rc::try_retain(L, value)) {
				rc::release(L, key);
				return false;
			}
			owned_key   = key;
			owned_value = value;
			return true;
		}

		internal_result next_internal(vm* L, iterator_state& state);

		internal_result next_collection(vm* L, iterator_state& state) {
			internal_result result{};
			if (state.exhausted)
				return result;

			shared::recursive_guard guard(L, state.target.is_gc() ? state.target.as_gc() : nullptr);
			if (!collection_is_unchanged_unlocked(state)) {
				L->error("collection mutated during iteration");
				result.next.ok = false;
				return result;
			}

			if (state.target.is_arr()) {
				array* source = state.target.as_arr();
				if (state.position == source->length) {
					state.exhausted = true;
					return result;
				}
				any key   = any(number(state.position));
				any value = source->begin()[state.position++];
				if (!retain_pair(L, key, value, result.key, result.next.value)) {
					result.next.ok = false;
					return result;
				}
				result.next.done = false;
				return result;
			}

			if (state.target.is_tarr()) {
				typed_array* source = state.target.as_tarr();
				if (state.position == source->length) {
					state.exhausted = true;
					return result;
				}
				result.key        = any(number(state.position));
				result.next.value = source->get(L, state.position++);
				if (result.next.value.is_exc()) {
					result.next.value = nil;
					result.next.ok    = false;
					return result;
				}
				result.next.done = false;
				return result;
			}

			if (state.target.is_str()) {
				string* source = state.target.as_str();
				if (state.position == source->length) {
					state.exhausted = true;
					return result;
				}
				result.key        = any(number(state.position));
				result.next.value = any(number(static_cast<uint8_t>(source->data[state.position++])));
				result.next.done  = false;
				return result;
			}

			table* source = state.target.as_tbl();
			while (state.position != source->realsize()) {
				const table_entry& entry = source->begin()[state.position++];
				if (entry.key == nil)
					continue;
				if (!retain_pair(L, entry.key, entry.value, result.key, result.next.value)) {
					result.next.ok = false;
					return result;
				}
				result.next.done = false;
				return result;
			}
			state.exhausted = true;
			return result;
		}

		internal_result next_range(vm* L, iterator_state& state) {
			internal_result result{};
			if (state.exhausted)
				return result;

			if (!state.open_end) {
				const bool past = state.step > 0 ? (state.inclusive ? state.current > state.end : state.current >= state.end)
															: (state.inclusive ? state.current < state.end : state.current <= state.end);
				if (past) {
					state.exhausted = true;
					return result;
				}
			}
			if (!std::isfinite(state.current)) {
				L->error("range arithmetic produced a non-finite value");
				result.next.ok = false;
				return result;
			}

			const number value = state.current;
			if (!state.open_end && state.inclusive && value == state.end) {
				state.exhausted = true;
			} else {
				const number next = value + state.step;
				if (next == value) {
					L->error("range step makes no numeric progress");
					result.next.ok = false;
					return result;
				}
				state.current = next;
			}
			result.key        = any(number(state.position++));
			result.next.value = any(value);
			result.next.done  = false;
			return result;
		}

		internal_result next_forward(vm* L, iterator_state& state) {
			internal_result result{};
			if (state.exhausted)
				return result;
			result.next = iterator_next(L, state.target);
			if (!result.next.ok)
				return result;
			if (result.next.done) {
				state.exhausted = true;
				return result;
			}
			result.key = any(number(state.position++));
			return result;
		}

		internal_result next_map(vm* L, iterator_state& state) {
			internal_result result{};
			if (state.exhausted)
				return result;
			iterator_state* source = state_from_value(state.target);
			if (!source) {
				L->error("invalid map iterator source");
				result.next.ok = false;
				return result;
			}

			internal_result input = next_internal(L, *source);
			if (!input.next.ok)
				return input;
			if (input.next.done) {
				state.exhausted = true;
				return input;
			}
			if (!L->vm_stack_headroom_available(1 + FRAME_SIZE)) [[unlikely]] {
				L->vm_stack_exhausted();
				rc::release(L, input.key);
				rc::release(L, input.next.value);
				result.next.ok = false;
				return result;
			}

			L->push_stack(input.next.value);
			any mapped = L->call(1, any(state.callback));
			rc::release(L, input.key);
			rc::release(L, input.next.value);
			if (mapped.is_exc()) {
				result.next.ok = false;
				return result;
			}
			if (!cursor_is_valid(L, *source)) {
				rc::release(L, mapped);
				result.next.ok = false;
				return result;
			}
			result.key        = any(number(state.position++));
			result.next.value = mapped;
			result.next.done  = false;
			return result;
		}

		internal_result next_filter(vm* L, iterator_state& state) {
			internal_result result{};
			if (state.exhausted)
				return result;
			iterator_state* source = state_from_value(state.target);
			if (!source) {
				L->error("invalid filter iterator source");
				result.next.ok = false;
				return result;
			}

			while (true) {
				internal_result input = next_internal(L, *source);
				if (!input.next.ok)
					return input;
				if (input.next.done) {
					state.exhausted = true;
					return input;
				}
				if (!L->vm_stack_headroom_available(1 + FRAME_SIZE)) [[unlikely]] {
					L->vm_stack_exhausted();
					rc::release(L, input.key);
					rc::release(L, input.next.value);
					result.next.ok = false;
					return result;
				}

				L->push_stack(input.next.value);
				any accepted = L->call(1, any(state.callback));
				if (accepted.is_exc()) {
					rc::release(L, input.key);
					rc::release(L, input.next.value);
					result.next.ok = false;
					return result;
				}
				const bool keep = accepted.coerce_bool();
				rc::release(L, accepted);
				if (!cursor_is_valid(L, *source)) {
					rc::release(L, input.key);
					rc::release(L, input.next.value);
					result.next.ok = false;
					return result;
				}
				rc::release(L, input.key);
				if (keep) {
					result.key        = any(number(state.position++));
					result.next.value = input.next.value;
					result.next.done  = false;
					return result;
				}
				rc::release(L, input.next.value);
			}
		}

		internal_result next_internal(vm* L, iterator_state& state) {
			switch (state.kind) {
				case iterator_kind::collection:
					return next_collection(L, state);
				case iterator_kind::forward:
					return next_forward(L, state);
				case iterator_kind::range:
					return next_range(L, state);
				case iterator_kind::map:
					return next_map(L, state);
				case iterator_kind::filter:
					return next_filter(L, state);
			}
			assume_unreachable();
		}

		object* create_cursor(vm* L, any_t source) {
			if (!rc::check_store(L, source))
				return nullptr;
			const bool builtin_collection = source.is_arr() || source.is_tarr() || source.is_tbl() || source.is_str();
			if (builtin_collection && !has_trait(source, trait::next))
				return create_state_object(L, L->iterator_class, iterator_kind::collection, source);
			if (state_from_value(source) || is_coroutine(L, source) || has_trait(source, trait::next))
				return create_state_object(L, L->iterator_class, iterator_kind::forward, source);
			L->error("cannot iterate %s", type_names[static_cast<uint8_t>(source.type())]);
			return nullptr;
		}

		next_result decode_script_pair(vm* L, any pair) {
			next_result result{};
			if (!pair.is_arr()) {
				rc::release(L, pair);
				L->error("next trait must return [value, done]");
				result.ok = false;
				return result;
			}

			bool malformed = false;
			bool retained  = false;
			{
				shared::recursive_guard guard(L, pair.as_arr());
				if (pair.as_arr()->length != 2 || !pair.as_arr()->begin()[1].is_bool()) {
					malformed = true;
				} else {
					any value = pair.as_arr()->begin()[0];
					retained  = rc::try_retain(L, value);
					if (retained) {
						result.value = value;
						result.done  = pair.as_arr()->begin()[1].as_bool();
					}
				}
			}
			rc::release(L, pair);
			if (malformed)
				L->error("next trait must return [value, done]");
			if (malformed || !retained)
				result.ok = false;
			return result;
		}

		any_t pack_next_result(vm* L, next_result result) {
			if (!result.ok)
				return exception_marker;
			array* pair = array::create(L, 0, 2);
			if (!pair->push(L, result.value) || !pair->push(L, any(result.done))) {
				rc::release(L, result.value);
				rc::release(L, pair);
				return exception_marker;
			}
			rc::release(L, result.value);
			return L->take(pair);
		}

		any_t LI_CC native_object_next(vm* L, any* args, slot_t n) {
			if (n != 0 || !state_from_value(args[1]))
				return L->error("iterator next expects an iterator receiver");
			return pack_next_result(L, iterator_next(L, args[1]));
		}

		any_t LI_CC native_next(vm* L, any* args, slot_t n) {
			if (n != 1)
				return L->error("iterator.next expects one iterator");
			return pack_next_result(L, iterator_next(L, args[0]));
		}

		any_t LI_CC native_range_create(vm* L, any* args, slot_t n) {
			if (n < 1 || n > 4)
				return L->error("range.create expects start, optional end/step, and optional inclusive flag");
			const any start_value     = args[0];
			const any end_value       = n >= 2 ? args[-1] : any(nil);
			const any step_value      = n >= 3 ? args[-2] : any(nil);
			const any inclusive_value = n >= 4 ? args[-3] : any(false);
			if (!start_value.is_num() || !std::isfinite(start_value.as_num()))
				return L->error("range start must be a finite number");
			if (end_value != nil && (!end_value.is_num() || !std::isfinite(end_value.as_num())))
				return L->error("range end must be nil or a finite number");
			if (step_value != nil && (!step_value.is_num() || !std::isfinite(step_value.as_num()) || step_value.as_num() == 0))
				return L->error("range step must be a finite nonzero number");
			if (!inclusive_value.is_bool())
				return L->error("range inclusive flag must be boolean");

			object* instance = create_state_object(L, L->range_class, iterator_kind::range);
			if (!instance)
				return exception_marker;
			iterator_state* state = state_from_object(instance);
			state->current        = start_value.as_num();
			state->open_end       = end_value == nil;
			state->end            = state->open_end ? 0 : end_value.as_num();
			state->step           = step_value == nil ? 1 : step_value.as_num();
			state->inclusive      = inclusive_value.as_bool();
			return L->take(instance);
		}

		any_t make_adapter(vm* L, any_t source, any_t callback, iterator_kind kind) {
			if (!callback.is_fn())
				return L->error(kind == iterator_kind::map ? "iterator.map expects an iterator and function" : "iterator.filter expects an iterator and function");
			if (!rc::check_store(L, callback))
				return exception_marker;
			object* cursor = create_cursor(L, source);
			if (!cursor)
				return exception_marker;
			object* adapter = create_state_object(L, L->iterator_class, kind, any(cursor), callback.as_fn());
			rc::release(L, cursor);
			if (!adapter)
				return exception_marker;
			return L->take(adapter);
		}

		any_t LI_CC native_map(vm* L, any* args, slot_t n) {
			if (n != 2)
				return L->error("iterator.map expects an iterator and function");
			return make_adapter(L, args[0], args[-1], iterator_kind::map);
		}

		any_t LI_CC native_filter(vm* L, any* args, slot_t n) {
			if (n != 2)
				return L->error("iterator.filter expects an iterator and function");
			return make_adapter(L, args[0], args[-1], iterator_kind::filter);
		}

		vclass* create_iterator_class(vm* L, const char* name, function* next_method) {
			constexpr std::size_t storage_size = sizeof(iterator_state) + alignof(iterator_state) - 1;
			string*               hidden_key   = string::create(L, "@iterator-state");
			field_pair            hidden_field{
				 .key = hidden_key,
				 .value =
					  field_info{
							.ty     = type::exc,
							.offset = uint32_t(storage_size - sizeof(any)),
					  },
			};
			std::array<uint8_t, storage_size> defaults{};
			string*                           class_name = string::create(L, name);
			vclass* cl  = vclass::create(L, class_name, std::span<const field_pair>{&hidden_field, 1}, defaults, {}, nullptr, reserve_class_identity(L));
			cl->cxx_tid = util::type_id_v<iterator_state>;
			cl->traits  = L->alloc<trait_set>();
			cl->traits->methods[size_t(trait::next)] = next_method;
			rc::retain(next_method);
			rc::release(L, hidden_key);
			rc::release(L, class_name);
			return cl;
		}
	}

	next_result iterator_next(vm* L, any_t iterator) {
		if (iterator_state* state = state_from_value(iterator)) {
			internal_result result = next_internal(L, *state);
			rc::release(L, result.key);
			return result.next;
		}
		if (is_coroutine(L, iterator))
			return coroutine_next(L, iterator);
		if (!has_trait(iterator, trait::next)) {
			L->error("value is not an iterator");
			return next_result{.ok = false};
		}
		any pair = invoke_trait(L, trait::next, iterator);
		if (pair.is_exc())
			return next_result{.ok = false};
		return decode_script_pair(L, pair);
	}

	any_t LI_CC iterator_step(vm* L, any_t iterable, any& state_slot, any& key_slot, any& value_slot) {
		if (!rc::try_retain(L, iterable))
			return exception_marker;

		iterator_state* state   = state_from_value(state_slot);
		object*         created = nullptr;
		if (!state) {
			created = create_cursor(L, iterable);
			if (!created) {
				rc::release(L, iterable);
				return exception_marker;
			}
			state = state_from_object(created);
		}

		internal_result result = next_internal(L, *state);
		if (!result.next.ok) {
			if (created)
				rc::release(L, created);
			rc::release(L, iterable);
			return exception_marker;
		}
		if (created)
			rc::replace_adopt(L, state_slot, any(created));
		if (result.next.done) {
			rc::release(L, result.key);
			rc::release(L, result.next.value);
			rc::release(L, iterable);
			return any(false);
		}

		rc::replace_adopt(L, key_slot, result.key);
		rc::replace_adopt(L, value_slot, result.next.value);
		rc::release(L, iterable);
		return any(true);
	}

	void lib::register_iterator(vm* L) {
		if (L->iterator_class || L->range_class)
			return;
		function* object_next = function::create(L, &native_object_next);
		L->iterator_class     = create_iterator_class(L, "iterator", object_next);
		L->range_class        = create_iterator_class(L, "range", object_next);
		util::export_as(L, "range.create", &native_range_create);
		util::export_as(L, "iterator.next", &native_next);
		util::export_as(L, "iterator.map", &native_map);
		util::export_as(L, "iterator.filter", &native_filter);
		rc::release(L, object_next);
	}
}
