#pragma once
#include <cstddef>
#include <cstdint>
#include <vm/state.hpp>

namespace li {
	enum class typed_array_kind : uint8_t {
		i8,
		u8,
		i16,
		u16,
		i32,
		u32,
		i64,
		u64,
		f32,
		f64,
		struct_elements,
	};

	constexpr msize_t typed_array_kind_size(typed_array_kind kind) {
		switch (kind) {
			case typed_array_kind::i8:
			case typed_array_kind::u8:
				return 1;
			case typed_array_kind::i16:
			case typed_array_kind::u16:
				return 2;
			case typed_array_kind::i32:
			case typed_array_kind::u32:
			case typed_array_kind::f32:
				return 4;
			case typed_array_kind::i64:
			case typed_array_kind::u64:
			case typed_array_kind::f64:
				return 8;
			case typed_array_kind::struct_elements:
				return 0;
		}
		assume_unreachable();
	}

	constexpr const char* typed_array_kind_name(typed_array_kind kind) {
		switch (kind) {
			case typed_array_kind::i8:
				return "i8";
			case typed_array_kind::u8:
				return "u8";
			case typed_array_kind::i16:
				return "i16";
			case typed_array_kind::u16:
				return "u16";
			case typed_array_kind::i32:
				return "i32";
			case typed_array_kind::u32:
				return "u32";
			case typed_array_kind::i64:
				return "i64";
			case typed_array_kind::u64:
				return "u64";
			case typed_array_kind::f32:
				return "f32";
			case typed_array_kind::f64:
				return "f64";
			case typed_array_kind::struct_elements:
				return "struct";
		}
		assume_unreachable();
	}

	struct typed_array_store : gc::leaf<typed_array_store> {
		alignas(uint64_t) std::byte entries[];
	};

	struct typed_array : gc::node<typed_array, type_typed_array> {
		static typed_array* create(vm* L, typed_array_kind kind, msize_t length = 0, msize_t capacity = 0);
		static typed_array* create(vm* L, vclass* element_class, msize_t length = 0, msize_t capacity = 0);

		typed_array_store* storage          = nullptr;
		msize_t            length           = 0;
		msize_t            capacity         = 0;
		uint64_t           mutation_version = 0;
		typed_array_kind   element_kind     = typed_array_kind::i8;
		vclass*            element_class    = nullptr;
		msize_t            element_stride   = 0;

		msize_t          element_size() const { return element_kind == typed_array_kind::struct_elements ? element_stride : typed_array_kind_size(element_kind); }
		std::byte*       data() { return storage ? storage->entries : nullptr; }
		const std::byte* data() const { return storage ? storage->entries : nullptr; }
		std::byte*       element_data(msize_t index) { return element_stride ? data() + size_t(index) * element_stride : data(); }
		const std::byte* element_data(msize_t index) const { return element_stride ? data() + size_t(index) * element_stride : data(); }
		msize_t          size() const { return length; }

		typed_array* duplicate(vm* L) const;

		void  reserve(vm* L, msize_t requested_capacity);
		void  resize(vm* L, msize_t requested_length);
		any_t reserve(vm* L, any_t requested_capacity);
		any_t resize(vm* L, any_t requested_length);
		any_t fill(vm* L, any_t value);

		any_t get(vm* L, msize_t index) const;
		any_t get(vm* L, any_t index) const;
		any_t set(vm* L, msize_t index, any_t value);
		any_t set(vm* L, any_t index, any_t value);

		bool is_struct_array() const { return element_kind == typed_array_kind::struct_elements; }
	};

	static_assert(alignof(typed_array_store) >= alignof(uint64_t));
}

namespace li::gc {
	void destroy(vm* L, typed_array* value);
}
