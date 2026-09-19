#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vm/inline_cache.hpp>
#include <vm/object.hpp>
#include <vm/runtime.hpp>
#include <vm/table.hpp>
#include <vm/traits.hpp>

namespace li {
	namespace {
		bool table_key_equals(any_t lhs, any_t rhs) {
			if (lhs.value == rhs.value)
				return true;
			if (lhs.is_str() && rhs.is_str())
				return string_value_equals(lhs.as_str(), rhs.as_str());
			return canonicalize_zero_bits(lhs.value) == canonicalize_zero_bits(rhs.value);
		}

		bool is_cached_field_type(type value) { return value == type::i1 || is_integer_data(value) || is_floating_point_data(value); }

		template<typename T>
		bool is_valid_integer(number value) {
			if (!std::isfinite(value) || value != std::trunc(value))
				return false;
			if constexpr (sizeof(T) == sizeof(int64_t))
				return value >= -0x1p63 && value < 0x1p63;
			return value >= number(std::numeric_limits<T>::lowest()) && value <= number(std::numeric_limits<T>::max());
		}

		bool value_fits_cached_field(any_t value, type storage) {
			switch (storage) {
				case type::i1:
					return value.is_bool();
				case type::i8:
					return value.is_num() && is_valid_integer<int8_t>(value.as_num());
				case type::i16:
					return value.is_num() && is_valid_integer<int16_t>(value.as_num());
				case type::i32:
					return value.is_num() && is_valid_integer<int32_t>(value.as_num());
				case type::i64:
					return value.is_num() && is_valid_integer<int64_t>(value.as_num());
				case type::f32:
				case type::f64:
					return value.is_num();
				default:
					return false;
			}
		}

		bool setter_is_intercepted(any_t target, any_t key) {
			if (!target.is_tbl() && !target.is_obj())
				return false;
			if (target.is_obj() && key.is_str() && class_has_property(target.as_obj()->cl, key.as_str()))
				return true;
			return trait_flag_enabled(target, trait::freeze) || has_trait(target, trait::set);
		}

		bool object_layout_matches(const object* instance, uint64_t identity, msize_t field_index, msize_t offset, type storage, any_t key) {
			if (!instance->cl || instance->cl->identity != identity || field_index >= instance->cl->num_fields)
				return false;
			const field_pair& field = instance->cl->fields()[field_index];
			if (field.value.is_static || field.value.is_dyn || field.value.offset != offset || field.value.ty != storage || !is_cached_field_type(storage))
				return false;
			if (!key.is_str() || !string_value_equals(field.key, key.as_str()))
				return false;
			const msize_t width = size_of_data(storage);
			return instance->data && offset <= instance->cl->object_length && width <= instance->cl->object_length - offset;
		}
	}

	struct inline_cache_access {
		enum class set_result {
			miss,
			hit,
			version_overflow,
		};

		template<typename F>
		static decltype(auto) locked(inline_cache& cache, F&& action) {
			if (!cache.owner_->shared_code_)
				return action();
			std::lock_guard guard(cache.owner_->mutex_);
			return action();
		}

		static void increment(uint64_t& value) {
			if (value != std::numeric_limits<uint64_t>::max())
				++value;
		}

		static void record_miss(inline_cache& cache) {
			locked(cache, [&] { increment(cache.misses_); });
		}

		static std::optional<any> get(inline_cache& cache, any_t target, any_t key) {
			return locked(cache, [&]() -> std::optional<any> {
				if (cache.kind_ == inline_cache::kind::table && target.is_tbl()) {
					table* value = target.as_tbl();
					if (value->shared || reinterpret_cast<uintptr_t>(value) != cache.table_receiver_identity_ ||
						 reinterpret_cast<uintptr_t>(value->node_list) != cache.table_nodes_identity_ || value->mask != cache.table_mask_ ||
						 cache.table_index_ >= value->realsize())
						return std::nullopt;
					table_entry* entry = &value->node_list->entries[cache.table_index_];
					if (reinterpret_cast<uintptr_t>(entry) != cache.table_entry_identity_ || !table_key_equals(entry->key, key))
						return std::nullopt;
					increment(cache.hits_);
					return entry->value;
				}
				if (cache.kind_ == inline_cache::kind::object && target.is_obj()) {
					object* value = target.as_obj();
					if (value->shared || !object_layout_matches(value, cache.class_identity_, cache.field_index_, cache.field_offset_, cache.field_type_, key))
						return std::nullopt;
					increment(cache.hits_);
					return any::load_from(value->data + cache.field_offset_, cache.field_type_);
				}
				return std::nullopt;
			});
		}

		static set_result set(inline_cache& cache, any_t target, any_t key, any_t replacement) {
			return locked(cache, [&] {
				if (cache.kind_ == inline_cache::kind::table && target.is_tbl()) {
					table* value = target.as_tbl();
					if (value->shared || reinterpret_cast<uintptr_t>(value) != cache.table_receiver_identity_ ||
						 reinterpret_cast<uintptr_t>(value->node_list) != cache.table_nodes_identity_ || value->mask != cache.table_mask_ ||
						 cache.table_index_ >= value->realsize())
						return set_result::miss;
					table_entry* entry = &value->node_list->entries[cache.table_index_];
					if (reinterpret_cast<uintptr_t>(entry) != cache.table_entry_identity_ || !table_key_equals(entry->key, key) || entry->value.is_gc() ||
						 replacement.is_gc())
						return set_result::miss;
					if (value->mutation_version == std::numeric_limits<uint64_t>::max())
						return set_result::version_overflow;
					entry->value = replacement;
					++value->mutation_version;
					increment(cache.hits_);
					return set_result::hit;
				}
				if (cache.kind_ == inline_cache::kind::object && target.is_obj()) {
					object* value = target.as_obj();
					if (value->shared || !object_layout_matches(value, cache.class_identity_, cache.field_index_, cache.field_offset_, cache.field_type_, key) ||
						 !value_fits_cached_field(replacement, cache.field_type_))
						return set_result::miss;
					any(replacement).store_at(value->data + cache.field_offset_, cache.field_type_);
					increment(cache.hits_);
					return set_result::hit;
				}
				return set_result::miss;
			});
		}

		static void populate(inline_cache& cache, any_t target, any_t key, bool raw) {
			if (target.is_tbl()) {
				table* value = target.as_tbl();
				if (value->shared || key == nil || !value->node_list)
					return;
				for (table_entry& entry : value->find(key.hash())) {
					if (!table_key_equals(entry.key, key))
						continue;
					const size_t index = size_t(&entry - value->begin());
					locked(cache, [&] {
						cache.kind_                    = inline_cache::kind::table;
						cache.table_receiver_identity_ = reinterpret_cast<uintptr_t>(value);
						cache.table_nodes_identity_    = reinterpret_cast<uintptr_t>(value->node_list);
						cache.table_entry_identity_    = reinterpret_cast<uintptr_t>(&entry);
						cache.table_mask_              = value->mask;
						cache.table_index_             = index;
					});
					return;
				}
				return;
			}

			if (!target.is_obj() || target.as_obj()->shared || !key.is_str())
				return;
			object* value = target.as_obj();
			if (!value->cl || (!raw && class_has_property(value->cl, key.as_str())))
				return;
			auto fields = value->cl->fields();
			for (size_t index = 0; index != fields.size(); ++index) {
				const field_pair& field = fields[index];
				if (!string_value_equals(field.key, key.as_str()))
					continue;
				if (field.value.is_static || field.value.is_dyn || !is_cached_field_type(field.value.ty))
					return;
				const msize_t width = size_of_data(field.value.ty);
				if (!value->data || field.value.offset > value->cl->object_length || width > value->cl->object_length - field.value.offset)
					return;
				locked(cache, [&] {
					cache.kind_           = inline_cache::kind::object;
					cache.class_identity_ = value->cl->identity;
					cache.field_index_    = msize_t(index);
					cache.field_offset_   = field.value.offset;
					cache.field_type_     = field.value.ty;
				});
				return;
			}
		}
	};

	inline_cache_array::inline_cache_array(size_t count, bool shared_code)
		 : shared_code_(shared_code), entries_(count ? std::make_unique<inline_cache[]>(count) : nullptr), count_(count) {
		for (size_t index = 0; index != count_; ++index)
			entries_[index].owner_ = this;
	}

	inline_cache_statistics inline_cache_array::statistics() const {
		const auto collect = [&] {
			inline_cache_statistics result{.sites = count_};
			for (size_t index = 0; index != count_; ++index) {
				const inline_cache& cache = entries_[index];
				result.hits               = std::min(std::numeric_limits<uint64_t>::max() - result.hits, cache.hits_) + result.hits;
				result.misses             = std::min(std::numeric_limits<uint64_t>::max() - result.misses, cache.misses_) + result.misses;
			}
			return result;
		};
		if (!shared_code_)
			return collect();
		std::lock_guard guard(mutex_);
		return collect();
	}

	namespace {
		any_t cached_get(vm* L, inline_cache* cache, any_t target, any_t key, bool raw) {
			if (!cache)
				return raw ? runtime::field_get_raw(L, target, key) : runtime::field_get(L, target, key);
			const bool property = !raw && target.is_obj() && key.is_str() && class_has_property(target.as_obj()->cl, key.as_str());
			if (!property) {
				if (std::optional<any> result = inline_cache_access::get(*cache, target, key))
					return L->ok(*result);
			}

			inline_cache_access::record_miss(*cache);
			any result = raw ? runtime::field_get_raw(L, target, key) : runtime::field_get(L, target, key);
			if (!result.is_exc())
				inline_cache_access::populate(*cache, target, key, raw);
			return result;
		}

		any_t cached_set(vm* L, inline_cache* cache, any_t target, any_t key, any_t value, bool raw) {
			if (!cache)
				return raw ? runtime::field_set_raw(L, target, key, value) : runtime::field_set(L, target, key, value);
			if (!(target.is_gc() && target.as_gc()->shared) && (raw || !setter_is_intercepted(target, key))) {
				switch (inline_cache_access::set(*cache, target, key, value)) {
					case inline_cache_access::set_result::hit:
						return L->ok();
					case inline_cache_access::set_result::version_overflow:
						L->panic("table mutation version overflow");
					case inline_cache_access::set_result::miss:
						break;
				}
			}

			inline_cache_access::record_miss(*cache);
			any result = raw ? runtime::field_set_raw(L, target, key, value) : runtime::field_set(L, target, key, value);
			if (!result.is_exc())
				inline_cache_access::populate(*cache, target, key, raw);
			return result;
		}
	}

	any_t LI_CC inline_cache_field_get(vm* L, inline_cache* cache, any_t target, any_t key) { return cached_get(L, cache, target, key, false); }

	any_t LI_CC inline_cache_field_get_raw(vm* L, inline_cache* cache, any_t target, any_t key) { return cached_get(L, cache, target, key, true); }

	any_t LI_CC inline_cache_field_set(vm* L, inline_cache* cache, any_t target, any_t key, any_t value) {
		return cached_set(L, cache, target, key, value, false);
	}

	any_t LI_CC inline_cache_field_set_raw(vm* L, inline_cache* cache, any_t target, any_t key, any_t value) {
		return cached_set(L, cache, target, key, value, true);
	}
}
