#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>
#include <vm/object.hpp>
#include <vm/rc.hpp>
#include <vm/shared.hpp>
#include <vm/typed_array.hpp>

namespace li {
	namespace {
		constexpr number maximum_safe_integer   = 9007199254740991.0;
		constexpr number f32_overflow_threshold = 0x1.ffffffp127;

		enum class encode_status {
			ok,
			nonnumeric,
			nonfinite,
			fractional,
			out_of_range,
		};

		template<typename T>
		T load_native(const void* source) {
			T value;
			std::memcpy(&value, source, sizeof(value));
			return value;
		}

		template<typename T>
		void store_native(void* destination, T value) {
			std::memcpy(destination, &value, sizeof(value));
		}

		template<typename T>
		encode_status encode_integer(number value, std::byte* destination) {
			if (!std::isfinite(value))
				return encode_status::nonfinite;
			if (value != std::trunc(value))
				return encode_status::fractional;

			number minimum = number(std::numeric_limits<T>::lowest());
			number maximum = number(std::numeric_limits<T>::max());
			if constexpr (sizeof(T) == sizeof(uint64_t)) {
				if constexpr (std::is_unsigned_v<T>)
					minimum = 0;
				else
					minimum = -maximum_safe_integer;
				maximum = maximum_safe_integer;
			}
			if (value < minimum || value > maximum)
				return encode_status::out_of_range;

			store_native(destination, static_cast<T>(value));
			return encode_status::ok;
		}

		encode_status encode_value(typed_array_kind kind, any_t value, std::byte* destination) {
			if (!value.is_num())
				return encode_status::nonnumeric;

			const number input = value.as_num();

			switch (kind) {
				case typed_array_kind::i8:
					return encode_integer<int8_t>(input, destination);
				case typed_array_kind::u8:
					return encode_integer<uint8_t>(input, destination);
				case typed_array_kind::i16:
					return encode_integer<int16_t>(input, destination);
				case typed_array_kind::u16:
					return encode_integer<uint16_t>(input, destination);
				case typed_array_kind::i32:
					return encode_integer<int32_t>(input, destination);
				case typed_array_kind::u32:
					return encode_integer<uint32_t>(input, destination);
				case typed_array_kind::i64:
					return encode_integer<int64_t>(input, destination);
				case typed_array_kind::u64:
					return encode_integer<uint64_t>(input, destination);
				case typed_array_kind::f32: {
					float rounded;
					if (std::isnan(input)) {
						rounded = std::numeric_limits<float>::quiet_NaN();
					} else if (input >= f32_overflow_threshold) {
						rounded = std::numeric_limits<float>::infinity();
					} else if (input > number(std::numeric_limits<float>::max())) {
						rounded = std::numeric_limits<float>::max();
					} else if (input <= -f32_overflow_threshold) {
						rounded = -std::numeric_limits<float>::infinity();
					} else if (input < number(std::numeric_limits<float>::lowest())) {
						rounded = std::numeric_limits<float>::lowest();
					} else {
						rounded = static_cast<float>(input);
					}
					store_native(destination, rounded);
					return encode_status::ok;
				}
				case typed_array_kind::f64:
					store_native(destination, input);
					return encode_status::ok;
				case typed_array_kind::struct_elements:
					break;
			}
			assume_unreachable();
		}

		number decode_value(typed_array_kind kind, const std::byte* source) {
			switch (kind) {
				case typed_array_kind::i8:
					return number(load_native<int8_t>(source));
				case typed_array_kind::u8:
					return number(load_native<uint8_t>(source));
				case typed_array_kind::i16:
					return number(load_native<int16_t>(source));
				case typed_array_kind::u16:
					return number(load_native<uint16_t>(source));
				case typed_array_kind::i32:
					return number(load_native<int32_t>(source));
				case typed_array_kind::u32:
					return number(load_native<uint32_t>(source));
				case typed_array_kind::i64:
					return number(load_native<int64_t>(source));
				case typed_array_kind::u64:
					return number(load_native<uint64_t>(source));
				case typed_array_kind::f32:
					return number(load_native<float>(source));
				case typed_array_kind::f64:
					return load_native<number>(source);
				case typed_array_kind::struct_elements:
					break;
			}
			assume_unreachable();
		}

		size_t checked_byte_count(vm* L, msize_t width, msize_t count) {
			if (width && size_t(count) > std::numeric_limits<size_t>::max() / width)
				L->panic("typed array allocation size overflow");
			return size_t(count) * width;
		}

		typed_array_store* allocate_store(vm* L, const typed_array* owner, size_t bytes) {
			return owner->shared ? shared::allocate<typed_array_store>(L, bytes) : L->alloc<typed_array_store>(bytes);
		}

		bool checked_count(any_t value, msize_t& result) {
			if (!value.is_num())
				return false;
			const number input = value.as_num();
			if (!std::isfinite(input) || input < 0 || input != std::trunc(input) || input > number(std::numeric_limits<msize_t>::max()))
				return false;
			result = static_cast<msize_t>(input);
			return true;
		}

		uint64_t next_mutation_version(vm* L, uint64_t current) {
			if (current == std::numeric_limits<uint64_t>::max())
				L->panic("typed array mutation version overflow");
			return current + 1;
		}

		any_t encode_error(vm* L, typed_array_kind kind, encode_status status) {
			switch (status) {
				case encode_status::nonnumeric:
					return L->error("%s typed array requires a numeric value", typed_array_kind_name(kind));
				case encode_status::nonfinite:
					return L->error("%s typed array rejects non-finite values", typed_array_kind_name(kind));
				case encode_status::fractional:
					return L->error("%s typed array requires an integer value", typed_array_kind_name(kind));
				case encode_status::out_of_range:
					return L->error("value is out of range for %s typed array", typed_array_kind_name(kind));
				case encode_status::ok:
					break;
			}
			assume_unreachable();
		}

		bool field_holds_reference(type ty) { return ty == type::any || is_gc_data(ty); }

		void retain_struct_slot(vclass* cl, const std::byte* slot) {
			for (const field_pair& field : cl->fields()) {
				if (!field.value.is_static && field_holds_reference(field.value.ty))
					rc::retain(any::load_from(slot + field.value.offset, field.value.ty));
			}
		}

		void release_struct_slot(vm* L, vclass* cl, std::byte* slot) {
			for (const field_pair& field : cl->fields()) {
				if (!field.value.is_static && field_holds_reference(field.value.ty))
					rc::release(L, any::load_from(slot + field.value.offset, field.value.ty));
			}
		}

		void initialize_struct_slot(vclass* cl, std::byte* slot, msize_t stride) {
			if (stride)
				std::memset(slot, 0, stride);
			if (cl->object_length)
				std::memcpy(slot, cl->default_space(), cl->object_length);
			retain_struct_slot(cl, slot);
		}

		void copy_struct_slot(vclass* cl, std::byte* destination, const std::byte* source, msize_t stride) {
			if (stride)
				std::memcpy(destination, source, stride);
			retain_struct_slot(cl, destination);
		}

		object* materialize_struct(vm* L, vclass* cl, const std::byte* slot) {
			object* result      = L->alloc<object>(cl->object_length);
			result->cl          = cl;
			result->data        = result->context;
			result->gc_hook     = nullptr;
			result->finalizable = true;
			result->type_id     = cl->vm_tid;
			rc::retain(result->cl);
			if (cl->object_length)
				std::memcpy(result->data, slot, cl->object_length);
			retain_struct_slot(cl, reinterpret_cast<const std::byte*>(result->data));
			return result;
		}

		bool replace_struct_slot(vm* L, typed_array* owner, msize_t element_index, const object* source) {
			const auto fields = owner->element_class->fields();
			if (!owner->shared) {
				if (element_index >= owner->length) {
					L->error("out-of-boundaries typed array access");
					return false;
				}
				std::byte* destination    = owner->element_data(element_index);
				size_t     retained_until = 0;
				for (; retained_until != fields.size(); ++retained_until) {
					const field_pair& field = fields[retained_until];
					if (field.value.is_static || !field_holds_reference(field.value.ty))
						continue;
					if (!rc::try_retain(L, any::load_from(source->data + field.value.offset, field.value.ty))) {
						for (size_t index = 0; index != retained_until; ++index) {
							const field_pair& retained = fields[index];
							if (!retained.value.is_static && field_holds_reference(retained.value.ty))
								rc::release(L, any::load_from(source->data + retained.value.offset, retained.value.ty));
						}
						return false;
					}
				}
				for (const field_pair& field : fields) {
					if (field.value.is_static)
						continue;
					if (!field_holds_reference(field.value.ty)) {
						std::memcpy(destination + field.value.offset, source->data + field.value.offset, size_of_data(field.value.ty));
						continue;
					}
					any previous = any::load_from(destination + field.value.offset, field.value.ty);
					any value    = any::load_from(source->data + field.value.offset, field.value.ty);
					value.store_at(destination + field.value.offset, field.value.ty);
					rc::release(L, previous);
				}
				owner->mutation_version = next_mutation_version(L, owner->mutation_version);
				return true;
			}

			std::vector<shared::prepared_value> prepared(fields.size());
			for (size_t index = 0; index != fields.size(); ++index) {
				const field_pair& field = fields[index];
				if (field.value.is_static || !field_holds_reference(field.value.ty))
					continue;
				prepared[index] = shared::prepare_store(L, owner, any::load_from(source->data + field.value.offset, field.value.ty));
				if (!prepared[index].ok) {
					for (shared::prepared_value& value : prepared)
						shared::finish_store(L, value);
					return false;
				}
			}
			size_t retained_until = 0;
			for (; retained_until != fields.size(); ++retained_until) {
				if (prepared[retained_until].ok && !rc::try_retain(L, prepared[retained_until].value)) {
					for (size_t index = 0; index != retained_until; ++index) {
						if (prepared[index].ok)
							rc::release(L, prepared[index].value);
					}
					for (shared::prepared_value& value : prepared)
						shared::finish_store(L, value);
					return false;
				}
			}
			shared::recursive_guard guard(L, owner);
			if (element_index >= owner->length) {
				for (size_t index = 0; index != fields.size(); ++index) {
					if (prepared[index].ok)
						rc::release(L, prepared[index].value);
				}
				for (shared::prepared_value& value : prepared)
					shared::finish_store(L, value);
				L->error("out-of-boundaries typed array access");
				return false;
			}
			std::byte* destination = owner->element_data(element_index);
			for (size_t index = 0; index != fields.size(); ++index) {
				const field_pair& field = fields[index];
				if (field.value.is_static)
					continue;
				if (!field_holds_reference(field.value.ty)) {
					std::memcpy(destination + field.value.offset, source->data + field.value.offset, size_of_data(field.value.ty));
					continue;
				}
				any previous = any::load_from(destination + field.value.offset, field.value.ty);
				prepared[index].value.store_at(destination + field.value.offset, field.value.ty);
				rc::release(L, previous);
			}
			for (shared::prepared_value& value : prepared)
				shared::finish_store(L, value);
			owner->mutation_version = next_mutation_version(L, owner->mutation_version);
			return true;
		}
	}

	typed_array* typed_array::create(vm* L, typed_array_kind kind, msize_t requested_length, msize_t requested_capacity) {
		if (kind == typed_array_kind::struct_elements)
			L->panic("struct typed array requires an element class");
		const msize_t actual_capacity = std::max(requested_length, requested_capacity);
		const msize_t width           = typed_array_kind_size(kind);
		const size_t  bytes           = checked_byte_count(L, width, actual_capacity);

		typed_array* result      = L->alloc<typed_array>();
		result->element_kind     = kind;
		result->element_stride   = width;
		result->length           = requested_length;
		result->capacity         = actual_capacity;
		result->mutation_version = 0;
		if (bytes) {
			result->storage = L->alloc<typed_array_store>(bytes);
			std::memset(result->storage->entries, 0, bytes);
		}
		return result;
	}

	typed_array* typed_array::create(vm* L, vclass* cl, msize_t requested_length, msize_t requested_capacity) {
		if (!cl || !cl->value_semantics)
			L->panic("struct typed array requires a value-semantics class");
		const msize_t alignment = alignof(any);
		if (cl->object_length > std::numeric_limits<msize_t>::max() - (alignment - 1))
			L->panic("struct typed array element stride overflow");
		const msize_t stride          = (cl->object_length + alignment - 1) & ~(alignment - 1);
		const msize_t actual_capacity = std::max(requested_length, requested_capacity);
		const size_t  bytes           = checked_byte_count(L, stride, actual_capacity);

		typed_array* result      = L->alloc<typed_array>();
		result->element_kind     = typed_array_kind::struct_elements;
		result->element_class    = cl;
		result->element_stride   = stride;
		result->length           = requested_length;
		result->capacity         = actual_capacity;
		result->mutation_version = 0;
		rc::retain(cl);
		if (bytes) {
			result->storage = L->alloc<typed_array_store>(bytes);
			std::memset(result->storage->entries, 0, bytes);
		}
		for (msize_t index = 0; index != requested_length; ++index)
			initialize_struct_slot(cl, result->element_data(index), stride);
		return result;
	}

	typed_array* typed_array::duplicate(vm* L) const {
		shared::recursive_guard guard(L, const_cast<typed_array*>(this));
		if (!is_struct_array()) {
			typed_array* result = create(L, element_kind, length, capacity);
			const size_t bytes  = checked_byte_count(L, element_size(), length);
			if (bytes)
				std::memcpy(result->data(), data(), bytes);
			return result;
		}

		typed_array* result = create(L, element_class, 0, capacity);
		result->length      = length;
		for (msize_t index = 0; index != length; ++index)
			copy_struct_slot(element_class, result->element_data(index), element_data(index), element_stride);
		return result;
	}

	void gc::destroy(vm* L, typed_array* value) {
		if (value->is_struct_array()) {
			for (msize_t index = 0; index != value->length; ++index)
				release_struct_slot(L, value->element_class, value->element_data(index));
		}
		typed_array_store* storage = value->storage;
		vclass*            cl      = value->element_class;
		value->storage             = nullptr;
		value->element_class       = nullptr;
		value->length              = 0;
		value->capacity            = 0;
		if (storage)
			rc::release(L, storage);
		rc::release(L, cl);
	}

	void typed_array::reserve(vm* L, msize_t requested_capacity) {
		shared::recursive_guard guard(L, this);
		if (requested_capacity <= capacity)
			return;

		const uint64_t     next_version    = next_mutation_version(L, mutation_version);
		const size_t       requested_bytes = checked_byte_count(L, element_size(), requested_capacity);
		const size_t       live_bytes      = checked_byte_count(L, element_size(), length);
		typed_array_store* replacement     = requested_bytes ? allocate_store(L, this, requested_bytes) : nullptr;
		if (requested_bytes)
			std::memset(replacement->entries, 0, requested_bytes);
		if (live_bytes)
			std::memcpy(replacement->entries, data(), live_bytes);

		typed_array_store* previous = storage;
		storage                     = replacement;
		capacity                    = requested_capacity;
		mutation_version            = next_version;
		if (previous)
			rc::release(L, previous);
	}

	void typed_array::resize(vm* L, msize_t requested_length) {
		shared::recursive_guard guard(L, this);
		const msize_t           previous_length = length;
		if (requested_length == previous_length)
			return;

		const uint64_t next_version = next_mutation_version(L, mutation_version);
		if (requested_length > capacity) {
			const size_t       requested_bytes = checked_byte_count(L, element_size(), requested_length);
			const size_t       live_bytes      = checked_byte_count(L, element_size(), previous_length);
			typed_array_store* replacement     = requested_bytes ? allocate_store(L, this, requested_bytes) : nullptr;
			if (requested_bytes)
				std::memset(replacement->entries, 0, requested_bytes);
			if (live_bytes)
				std::memcpy(replacement->entries, data(), live_bytes);
			if (is_struct_array()) {
				for (msize_t index = previous_length; index != requested_length; ++index) {
					std::byte* slot = element_stride ? replacement->entries + size_t(index) * element_stride : nullptr;
					initialize_struct_slot(element_class, slot, element_stride);
				}
			}

			typed_array_store* previous = storage;
			storage                     = replacement;
			length                      = requested_length;
			capacity                    = requested_length;
			mutation_version            = next_version;
			if (previous)
				rc::release(L, previous);
			return;
		}

		if (is_struct_array()) {
			if (requested_length > previous_length) {
				for (msize_t index = previous_length; index != requested_length; ++index)
					initialize_struct_slot(element_class, element_data(index), element_stride);
			} else {
				for (msize_t index = requested_length; index != previous_length; ++index) {
					std::byte* slot = element_data(index);
					release_struct_slot(L, element_class, slot);
					if (element_stride)
						std::memset(slot, 0, element_stride);
				}
			}
		} else if (requested_length > previous_length) {
			const size_t start = checked_byte_count(L, element_size(), previous_length);
			const size_t count = checked_byte_count(L, element_size(), requested_length - previous_length);
			std::memset(data() + start, 0, count);
		} else {
			const size_t start = checked_byte_count(L, element_size(), requested_length);
			const size_t count = checked_byte_count(L, element_size(), previous_length - requested_length);
			std::memset(data() + start, 0, count);
		}
		length           = requested_length;
		mutation_version = next_version;
	}

	any_t typed_array::reserve(vm* L, any_t requested_capacity) {
		msize_t count = 0;
		if (!checked_count(requested_capacity, count))
			return L->error("typed array capacity must be a non-negative integer");
		reserve(L, count);
		return L->ok();
	}

	any_t typed_array::resize(vm* L, any_t requested_length) {
		msize_t count = 0;
		if (!checked_count(requested_length, count))
			return L->error("typed array length must be a non-negative integer");
		resize(L, count);
		return L->ok();
	}

	any_t typed_array::fill(vm* L, any_t value) {
		if (is_struct_array()) {
			if (!value.is_obj() || !value.as_obj()->cl || !value.as_obj()->cl->value_semantics || value.as_obj()->cl->identity != element_class->identity)
				return L->error("struct typed array requires an instance of its element class");
			msize_t fill_length;
			{
				shared::recursive_guard guard(L, this);
				fill_length = length;
			}
			for (msize_t index = 0; index != fill_length; ++index) {
				if (!replace_struct_slot(L, this, index, value.as_obj()))
					return exception_marker;
			}
			return L->ok();
		}

		std::array<std::byte, sizeof(number)> encoded{};
		const encode_status                   status = encode_value(element_kind, value, encoded.data());
		if (status != encode_status::ok)
			return encode_error(L, element_kind, status);
		shared::recursive_guard guard(L, this);
		if (!length)
			return L->ok();

		const uint64_t next_version = next_mutation_version(L, mutation_version);
		const size_t   width        = element_size();
		for (msize_t index = 0; index != length; ++index)
			std::memcpy(data() + size_t(index) * width, encoded.data(), width);
		mutation_version = next_version;
		return L->ok();
	}

	any_t typed_array::get(vm* L, msize_t index) const {
		shared::recursive_guard guard(L, const_cast<typed_array*>(this));
		if (index >= length)
			return L->ok();
		if (is_struct_array())
			return L->take(materialize_struct(L, element_class, element_data(index)));
		const size_t offset = size_t(index) * element_size();
		return L->ok(any(decode_value(element_kind, data() + offset)));
	}

	any_t typed_array::get(vm* L, any_t index) const {
		msize_t checked = 0;
		if (!checked_count(index, checked))
			return L->error("typed array index must be a non-negative integer");
		return get(L, checked);
	}

	any_t typed_array::set(vm* L, msize_t index, any_t value) {
		if (is_struct_array()) {
			if (!value.is_obj() || !value.as_obj()->cl || !value.as_obj()->cl->value_semantics || value.as_obj()->cl->identity != element_class->identity)
				return L->error("struct typed array requires an instance of its element class");
			if (!replace_struct_slot(L, this, index, value.as_obj()))
				return exception_marker;
			return L->ok();
		}

		std::array<std::byte, sizeof(number)> encoded{};
		const encode_status                   status = encode_value(element_kind, value, encoded.data());
		if (status != encode_status::ok)
			return encode_error(L, element_kind, status);

		shared::recursive_guard guard(L, this);
		if (index >= length)
			return L->error("out-of-boundaries typed array access");
		const uint64_t next_version = next_mutation_version(L, mutation_version);
		const size_t   width        = element_size();
		std::memcpy(data() + size_t(index) * width, encoded.data(), width);
		mutation_version = next_version;
		return L->ok();
	}

	any_t typed_array::set(vm* L, any_t index, any_t value) {
		msize_t checked = 0;
		if (!checked_count(index, checked))
			return L->error("typed array index must be a non-negative integer");
		return set(L, checked, value);
	}
}
