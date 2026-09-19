#include <array>
#include <bit>
#include <cmath>
#include <lib/std.hpp>
#include <limits>
#include <util/user.hpp>
#include <vm/array.hpp>
#include <vm/iterator.hpp>
#include <vm/rc.hpp>
#include <vm/shared.hpp>
#include <vm/string.hpp>
#include <vm/table.hpp>
#include <vm/traits.hpp>
#include <vm/typed_array.hpp>

namespace li::lib {
	namespace {
		bool checked_count(any_t value, msize_t& result) {
			if (!value.is_num())
				return false;
			const number input = value.as_num();
			if (!std::isfinite(input) || input < 0 || input != std::trunc(input) || input > number(std::numeric_limits<msize_t>::max()))
				return false;
			result = static_cast<msize_t>(input);
			return true;
		}

		any_t table_reserve(vm* L, any* args, slot_t count) {
			if (count != 2 || !args[0].is_tbl())
				return L->error("table.reserve expects a table and capacity");
			if (trait_flag_enabled(args[0], trait::freeze))
				return L->error("modifying frozen table");

			msize_t capacity = 0;
			if (!checked_count(args[-1], capacity))
				return L->error("table capacity must be a non-negative integer");
			if (capacity > std::bit_floor(std::numeric_limits<msize_t>::max()))
				return L->error("table capacity is too large");

			args[0].as_tbl()->resize(L, capacity);
			return L->ok();
		}

		array* expect_array(vm* L, any* args, slot_t count, slot_t expected, const char* operation) {
			if (count != expected || !args[0].is_arr()) {
				L->error("array.%s expects an array%s", operation, expected == 1 ? "" : " and arguments");
				return nullptr;
			}
			return args[0].as_arr();
		}

		any_t array_reserve(vm* L, any* args, slot_t count) {
			array* value = expect_array(L, args, count, 2, "reserve");
			if (!value)
				return exception_marker;
			if (trait_flag_enabled(args[0], trait::freeze))
				return L->error("modifying frozen array");

			msize_t capacity = 0;
			if (!checked_count(args[-1], capacity))
				return L->error("array capacity must be a non-negative integer");
			value->reserve(L, capacity);
			return L->ok();
		}

		any_t array_resize(vm* L, any* args, slot_t count) {
			array* value = expect_array(L, args, count, 2, "resize");
			if (!value)
				return exception_marker;
			if (trait_flag_enabled(args[0], trait::freeze))
				return L->error("modifying frozen array");

			msize_t length = 0;
			if (!checked_count(args[-1], length))
				return L->error("array length must be a non-negative integer");
			value->resize(L, length);
			return L->ok();
		}

		any_t array_fill(vm* L, any* args, slot_t count) {
			if (count < 2 || count > 4 || !args[0].is_arr())
				return L->error("array.fill expects an array, value, and optional start/end bounds");
			if (trait_flag_enabled(args[0], trait::freeze))
				return L->error("modifying frozen array");

			array*  value = args[0].as_arr();
			msize_t start = 0;
			msize_t end   = value->length;
			if (count >= 3 && !checked_count(args[-2], start))
				return L->error("array fill start must be a non-negative integer");
			if (count == 4 && !checked_count(args[-3], end))
				return L->error("array fill end must be a non-negative integer");
			if (start > end || end > value->length)
				return L->error("array fill bounds are out of range");
			if (!value->fill(L, args[-1], start, end))
				return exception_marker;
			return L->ok();
		}

		msize_t collection_size_hint(vm* L, any_t source) {
			if (has_trait(source, trait::next))
				return 0;
			shared::recursive_guard guard(L, source.is_gc() ? source.as_gc() : nullptr);
			if (source.is_arr())
				return source.as_arr()->length;
			if (source.is_tarr())
				return source.as_tarr()->length;
			if (source.is_tbl())
				return source.as_tbl()->active_count;
			if (source.is_str())
				return source.as_str()->length;
			return 0;
		}

		struct mutation_watch {
			any_t    source;
			uint64_t version = 0;
			bool     enabled = false;

			mutation_watch() = default;
			explicit mutation_watch(vm* L, any_t value) : source(value) {
				enabled = !has_trait(source, trait::next) && (source.is_arr() || source.is_tarr() || source.is_tbl());
				if (!enabled)
					return;
				shared::recursive_guard guard(L, source.as_gc());
				if (source.is_arr())
					version = source.as_arr()->mutation_version;
				else if (source.is_tarr())
					version = source.as_tarr()->mutation_version;
				else
					version = source.as_tbl()->mutation_version;
			}

			bool validate(vm* L) const {
				if (!enabled)
					return true;
				shared::recursive_guard guard(L, source.as_gc());
				const bool              unchanged = source.is_arr()    ? source.as_arr()->mutation_version == version
																: source.is_tarr() ? source.as_tarr()->mutation_version == version
																						 : source.as_tbl()->mutation_version == version;
				if (!unchanged)
					L->error("collection mutated during iteration");
				return unchanged;
			}
		};

		struct functional_cursor {
			vm*            owner;
			any_t          source;
			mutation_watch watch;
			any            state = nil;
			any            key   = nil;
			any            value = nil;

			functional_cursor(vm* L, any_t value) : owner(L), source(value), watch(L, value) {}
			~functional_cursor() {
				rc::release(owner, state);
				rc::release(owner, key);
				rc::release(owner, value);
			}

			void clear_item() {
				rc::clear(owner, key);
				rc::clear(owner, value);
			}

			bool next(vm* L, bool& exhausted) {
				clear_item();
				any advanced = iterator_step(L, source, state, key, value);
				if (advanced.is_exc())
					return false;
				exhausted = !advanced.as_bool();
				return true;
			}
		};

		bool validate_callback(vm* L, any_t callback, const char* operation) {
			if (callback.is_fn())
				return true;
			L->error("collections.%s expects a function callback", operation);
			return false;
		}

		bool consume_boolean_result(vm* L, any result, const char* operation, bool& value) {
			if (!result.is_bool()) {
				rc::release(L, result);
				L->error("collections.%s callback must return boolean", operation);
				return false;
			}
			value = result.as_bool();
			rc::release(L, result);
			return true;
		}

		bool callback_headroom(vm* L, size_t arguments) {
			if (L->vm_stack_headroom_available(arguments + FRAME_SIZE))
				return true;
			L->vm_stack_exhausted();
			return false;
		}

		any_t collections_map(vm* L, any* args, slot_t count) {
			vm_stack_guard stack_guard{L, args};
			if (count != 2)
				return L->error("collections.map expects an iterable and callback");
			const any source   = args[0];
			const any callback = args[-1];
			if (!validate_callback(L, callback, "map"))
				return exception_marker;

			functional_cursor cursor{L, source};
			array*            result = array::create(L, 0, collection_size_hint(L, source));
			while (true) {
				bool exhausted = false;
				if (!cursor.next(L, exhausted)) {
					rc::release(L, result);
					return exception_marker;
				}
				if (exhausted)
					break;
				if (!callback_headroom(L, 2)) {
					rc::release(L, result);
					return exception_marker;
				}

				L->push_stack(cursor.key);
				L->push_stack(cursor.value);
				any mapped = L->call(2, callback);
				if (mapped.is_exc()) {
					rc::release(L, result);
					return mapped;
				}
				if (!cursor.watch.validate(L)) {
					rc::release(L, mapped);
					rc::release(L, result);
					return exception_marker;
				}
				if (!result->push(L, mapped)) {
					rc::release(L, mapped);
					rc::release(L, result);
					return exception_marker;
				}
				rc::release(L, mapped);
			}
			return L->take(result);
		}

		any_t collections_filter(vm* L, any* args, slot_t count) {
			vm_stack_guard stack_guard{L, args};
			if (count != 2)
				return L->error("collections.filter expects an iterable and callback");
			const any source   = args[0];
			const any callback = args[-1];
			if (!validate_callback(L, callback, "filter"))
				return exception_marker;

			functional_cursor cursor{L, source};
			array*            result = array::create(L, 0, collection_size_hint(L, source));
			while (true) {
				bool exhausted = false;
				if (!cursor.next(L, exhausted)) {
					rc::release(L, result);
					return exception_marker;
				}
				if (exhausted)
					break;
				if (!callback_headroom(L, 2)) {
					rc::release(L, result);
					return exception_marker;
				}

				L->push_stack(cursor.key);
				L->push_stack(cursor.value);
				any predicate = L->call(2, callback);
				if (predicate.is_exc()) {
					rc::release(L, result);
					return predicate;
				}
				bool keep = false;
				if (!consume_boolean_result(L, predicate, "filter", keep)) {
					rc::release(L, result);
					return exception_marker;
				}
				if (!cursor.watch.validate(L)) {
					rc::release(L, result);
					return exception_marker;
				}
				if (keep && !result->push(L, cursor.value)) {
					rc::release(L, result);
					return exception_marker;
				}
			}
			return L->take(result);
		}

		any_t collections_reduce(vm* L, any* args, slot_t count) {
			vm_stack_guard stack_guard{L, args};
			if (count != 3)
				return L->error("collections.reduce expects an iterable, initial value, and callback");
			const any source   = args[0];
			const any callback = args[-2];
			if (!validate_callback(L, callback, "reduce"))
				return exception_marker;

			any accumulator = L->ok(args[-1]);
			if (accumulator.is_exc())
				return accumulator;

			functional_cursor cursor{L, source};
			while (true) {
				bool exhausted = false;
				if (!cursor.next(L, exhausted)) {
					rc::release(L, accumulator);
					return exception_marker;
				}
				if (exhausted)
					break;
				if (!callback_headroom(L, 3)) {
					rc::release(L, accumulator);
					return exception_marker;
				}

				L->push_stack(cursor.key);
				L->push_stack(cursor.value);
				L->push_stack(accumulator);
				any next = L->call(3, callback);
				if (next.is_exc()) {
					rc::release(L, accumulator);
					return next;
				}
				if (!cursor.watch.validate(L)) {
					rc::release(L, next);
					rc::release(L, accumulator);
					return exception_marker;
				}
				rc::release(L, accumulator);
				accumulator = next;
			}
			return accumulator;
		}

		any_t collections_each(vm* L, any* args, slot_t count) {
			vm_stack_guard stack_guard{L, args};
			if (count != 2)
				return L->error("collections.each expects an iterable and callback");
			const any source   = args[0];
			const any callback = args[-1];
			if (!validate_callback(L, callback, "each"))
				return exception_marker;

			functional_cursor cursor{L, source};
			while (true) {
				bool exhausted = false;
				if (!cursor.next(L, exhausted))
					return exception_marker;
				if (exhausted)
					break;
				if (!callback_headroom(L, 2))
					return exception_marker;

				L->push_stack(cursor.key);
				L->push_stack(cursor.value);
				any ignored = L->call(2, callback);
				if (ignored.is_exc())
					return ignored;
				rc::release(L, ignored);
				if (!cursor.watch.validate(L))
					return exception_marker;
			}
			return L->ok(source);
		}

		any_t collections_quantifier(vm* L, any* args, slot_t count, const char* operation, bool identity, bool stop_value) {
			vm_stack_guard stack_guard{L, args};
			if (count != 2)
				return L->error("collections.%s expects an iterable and callback", operation);
			const any source   = args[0];
			const any callback = args[-1];
			if (!validate_callback(L, callback, operation))
				return exception_marker;

			functional_cursor cursor{L, source};
			while (true) {
				bool exhausted = false;
				if (!cursor.next(L, exhausted))
					return exception_marker;
				if (exhausted)
					return L->ok(identity);
				if (!callback_headroom(L, 2))
					return exception_marker;

				L->push_stack(cursor.key);
				L->push_stack(cursor.value);
				any predicate = L->call(2, callback);
				if (predicate.is_exc())
					return predicate;
				bool matched = false;
				if (!consume_boolean_result(L, predicate, operation, matched))
					return exception_marker;
				if (!cursor.watch.validate(L))
					return exception_marker;
				if (matched == stop_value)
					return L->ok(stop_value);
			}
		}

		any_t collections_any(vm* L, any* args, slot_t count) { return collections_quantifier(L, args, count, "any", false, true); }
		any_t collections_all(vm* L, any* args, slot_t count) { return collections_quantifier(L, args, count, "all", true, false); }

		any_t collections_find(vm* L, any* args, slot_t count) {
			vm_stack_guard stack_guard{L, args};
			if (count != 2)
				return L->error("collections.find expects an iterable and callback");
			const any source   = args[0];
			const any callback = args[-1];
			if (!validate_callback(L, callback, "find"))
				return exception_marker;

			functional_cursor cursor{L, source};
			while (true) {
				bool exhausted = false;
				if (!cursor.next(L, exhausted))
					return exception_marker;
				if (exhausted)
					return L->ok();
				if (!callback_headroom(L, 2))
					return exception_marker;

				L->push_stack(cursor.key);
				L->push_stack(cursor.value);
				any predicate = L->call(2, callback);
				if (predicate.is_exc())
					return predicate;
				bool matched = false;
				if (!consume_boolean_result(L, predicate, "find", matched))
					return exception_marker;
				if (!cursor.watch.validate(L))
					return exception_marker;
				if (matched)
					return L->ok(cursor.value);
			}
		}

		array* collect_values(vm* L, any_t source) {
			functional_cursor cursor{L, source};
			array*            result = array::create(L, 0, collection_size_hint(L, source));
			while (true) {
				bool exhausted = false;
				if (!cursor.next(L, exhausted)) {
					rc::release(L, result);
					return nullptr;
				}
				if (exhausted)
					return result;
				if (!result->push(L, cursor.value)) {
					rc::release(L, result);
					return nullptr;
				}
			}
		}

		any_t collections_sort(vm* L, any* args, slot_t count) {
			vm_stack_guard stack_guard{L, args};
			if (count != 2)
				return L->error("collections.sort expects an iterable and comparator");
			const any source     = args[0];
			const any comparator = args[-1];
			if (!validate_callback(L, comparator, "sort"))
				return exception_marker;

			array* result = collect_values(L, source);
			if (!result)
				return exception_marker;
			for (msize_t position = 1; position < result->length; ++position) {
				msize_t current = position;
				while (current != 0) {
					if (!callback_headroom(L, 2)) {
						rc::release(L, result);
						return exception_marker;
					}
					any lhs = result->begin()[current];
					any rhs = result->begin()[current - 1];
					L->push_stack(rhs);
					L->push_stack(lhs);
					any comparison = L->call(2, comparator);
					if (comparison.is_exc()) {
						rc::release(L, result);
						return comparison;
					}
					bool before = false;
					if (!consume_boolean_result(L, comparison, "sort", before)) {
						rc::release(L, result);
						return exception_marker;
					}
					if (!before)
						break;
					std::swap(result->begin()[current], result->begin()[current - 1]);
					--current;
				}
			}
			return L->take(result);
		}

		any_t collections_keys(vm* L, any* args, slot_t count) {
			vm_stack_guard stack_guard{L, args};
			if (count != 1)
				return L->error("collections.keys expects one iterable");

			const any         source = args[0];
			functional_cursor cursor{L, source};
			array*            result = array::create(L, 0, collection_size_hint(L, source));
			while (true) {
				bool exhausted = false;
				if (!cursor.next(L, exhausted)) {
					rc::release(L, result);
					return exception_marker;
				}
				if (exhausted)
					break;
				if (!result->push(L, cursor.key)) {
					rc::release(L, result);
					return exception_marker;
				}
			}
			return L->take(result);
		}

		any_t collections_values(vm* L, any* args, slot_t count) {
			vm_stack_guard stack_guard{L, args};
			if (count != 1)
				return L->error("collections.values expects one iterable");
			array* result = collect_values(L, args[0]);
			if (!result)
				return exception_marker;
			return L->take(result);
		}

		struct zip_cursors {
			vm*                                  owner;
			slot_t                               count;
			std::array<any, MAX_ARGS>            state{};
			std::array<any, MAX_ARGS>            key{};
			std::array<any, MAX_ARGS>            value{};
			std::array<mutation_watch, MAX_ARGS> watch{};

			zip_cursors(vm* L, any* args, slot_t count) : owner(L), count(count) {
				for (slot_t index = 0; index != count; ++index)
					watch[size_t(index)] = mutation_watch(L, args[-index]);
			}
			~zip_cursors() {
				for (slot_t index = 0; index != count; ++index) {
					rc::release(owner, state[size_t(index)]);
					rc::release(owner, key[size_t(index)]);
					rc::release(owner, value[size_t(index)]);
				}
			}

			bool next(vm* L, slot_t index, any_t source, bool& exhausted) {
				rc::clear(L, key[size_t(index)]);
				rc::clear(L, value[size_t(index)]);
				any advanced = iterator_step(L, source, state[size_t(index)], key[size_t(index)], value[size_t(index)]);
				if (advanced.is_exc())
					return false;
				for (slot_t watched = 0; watched != count; ++watched) {
					if (!watch[size_t(watched)].validate(L))
						return false;
				}
				exhausted = !advanced.as_bool();
				return true;
			}
		};

		any_t collections_zip(vm* L, any* args, slot_t count) {
			vm_stack_guard stack_guard{L, args};
			if (count < 1 || count > MAX_ARGS)
				return L->error("collections.zip expects one or more iterables");

			zip_cursors cursors{L, args, count};
			array*      result = array::create(L);
			while (true) {
				array* row = array::create(L, 0, msize_t(count));
				for (slot_t index = 0; index != count; ++index) {
					bool exhausted = false;
					if (!cursors.next(L, index, args[-index], exhausted)) {
						rc::release(L, row);
						rc::release(L, result);
						return exception_marker;
					}
					if (exhausted) {
						rc::release(L, row);
						return L->take(result);
					}
					if (!row->push(L, cursors.value[size_t(index)])) {
						rc::release(L, row);
						rc::release(L, result);
						return exception_marker;
					}
				}
				if (!result->push(L, any(row))) {
					rc::release(L, row);
					rc::release(L, result);
					return exception_marker;
				}
				rc::release(L, row);
			}
		}
	}

	void register_collections(vm* L) {
		util::export_as(L, "table.reserve", table_reserve);
		util::export_as(L, "array.reserve", array_reserve);
		util::export_as(L, "array.resize", array_resize);
		util::export_as(L, "array.fill", array_fill);
		util::export_as(L, "collections.map", collections_map);
		util::export_as(L, "collections.filter", collections_filter);
		util::export_as(L, "collections.reduce", collections_reduce);
		util::export_as(L, "collections.each", collections_each);
		util::export_as(L, "collections.any", collections_any);
		util::export_as(L, "collections.all", collections_all);
		util::export_as(L, "collections.find", collections_find);
		util::export_as(L, "collections.sort", collections_sort);
		util::export_as(L, "collections.keys", collections_keys);
		util::export_as(L, "collections.values", collections_values);
		util::export_as(L, "collections.zip", collections_zip);
	}
}
