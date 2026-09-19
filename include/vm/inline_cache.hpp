#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vm/types.hpp>

namespace li {
	struct vm;

	struct inline_cache_statistics {
		uint64_t hits   = 0;
		uint64_t misses = 0;
		size_t   sites  = 0;
	};

	class inline_cache_array;

	class inline_cache {
	  public:
		inline_cache() = default;

	  private:
		friend class inline_cache_array;
		friend struct inline_cache_access;

		enum class kind : uint8_t {
			empty,
			table,
			object,
		};

		inline_cache_array* owner_  = nullptr;
		kind                kind_   = kind::empty;
		uint64_t            hits_   = 0;
		uint64_t            misses_ = 0;

		// Table hints are non-owning. Every hit revalidates the live receiver,
		// backing allocation, mask, slot index, address, and current key.
		uintptr_t table_receiver_identity_ = 0;
		uintptr_t table_nodes_identity_    = 0;
		uintptr_t table_entry_identity_    = 0;
		size_t    table_mask_              = 0;
		size_t    table_index_             = 0;

		// Class layouts are immutable. No class or key reference is retained.
		uint64_t class_identity_ = 0;
		msize_t  field_index_    = 0;
		msize_t  field_offset_   = 0;
		type     field_type_     = type::none;
	};

	class inline_cache_array {
	  public:
		explicit inline_cache_array(size_t count, bool shared_code);

		inline_cache_array(const inline_cache_array&)            = delete;
		inline_cache_array& operator=(const inline_cache_array&) = delete;

		[[nodiscard]] size_t                  size() const noexcept { return count_; }
		[[nodiscard]] inline_cache*           data() noexcept { return entries_.get(); }
		[[nodiscard]] inline_cache&           operator[](size_t index) noexcept { return entries_[index]; }
		[[nodiscard]] inline_cache_statistics statistics() const;

	  private:
		friend struct inline_cache_access;

		bool                            shared_code_ = false;
		mutable std::mutex              mutex_;
		std::unique_ptr<inline_cache[]> entries_;
		size_t                          count_ = 0;
	};

	// Generated-code helpers. Inputs are borrowed; successful getter results are
	// owned exactly like the generic runtime field helpers.
	any_t LI_CC inline_cache_field_get(vm* L, inline_cache* cache, any_t target, any_t key);
	any_t LI_CC inline_cache_field_get_raw(vm* L, inline_cache* cache, any_t target, any_t key);
	any_t LI_CC inline_cache_field_set(vm* L, inline_cache* cache, any_t target, any_t key, any_t value);
	any_t LI_CC inline_cache_field_set_raw(vm* L, inline_cache* cache, any_t target, any_t key, any_t value);
}
