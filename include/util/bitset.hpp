#pragma once
#include <algorithm>
#include <bit>
#include <util/common.hpp>
#include <vector>

namespace li::util {
	// Better bitset.
	//
	struct bitset {
		static constexpr size_t width = sizeof(size_t) * 8;
		static constexpr size_t npos  = ~size_t(0);

		// Data storage.
		//
		std::vector<size_t> data;
		size_t              real_length = 0;

		// Constructed by optional length.
		//
		bitset() = default;
		bitset(size_t n) { resize(n); }

		// Default copy.
		//
		bitset(const bitset&)            = default;
		bitset& operator=(const bitset&) = default;

		// Move via swap.
		//
		bitset(bitset&& o) noexcept { swap(o); }
		bitset& operator=(bitset&& o) noexcept {
			swap(o);
			return *this;
		};
		void swap(bitset& o) {
			std::swap(data, o.data);
			std::swap(real_length, o.real_length);
		}

		// Clears leftover at the last block.
		//
		void clear_leftover() {
			if (real_length)
				data.back() &= ~fill_bits(real_length & (width - 1));
		}

		// Resize/Clear/Shrink.
		//
		void resize(size_t n) {
			bool shrink = n < real_length;
			data.resize((n + width - 1) / width);
			real_length = n;

			if (shrink) {
				clear_leftover();
			}
		}
		void clear() {
			data.clear();
			real_length = 0;
		}
		void shrink_to_fit() { data.shrink_to_fit(); }

		// Fill/Flip.
		//
		void fill(bool x) {
			range::fill(data, x ? ~size_t(0) : 0);
			clear_leftover();
		}
		void flip() {
			for (auto& x : data)
				x = ~x;
			clear_leftover();
		}

		// Common size calculator.
		//
		size_t common_blocks(const bitset& o) const { return (std::min(real_length, o.real_length) + width - 1) / width; }

		// Mask checks, common size is used.
		//
		bool has_none(const bitset& o) const {
			size_t l = common_blocks(o);
			for (size_t i = 0; i != l; i++) {
				if (o.data[i] & data[i])
					return false;
			}
			return true;
		}
		bool has_all(const bitset& o) const {
			size_t l = common_blocks(o);
			for (size_t i = 0; i != l; i++) {
				if ((o.data[i] | data[i]) != data[i])
					return false;
			}
			return true;
		}

		// Checks if all zeroes/ones.
		//
		bool all(bool x) const {
			if (empty())
				return true;
			size_t n = data.size();
			size_t last_mask = fill_bits(real_length & (width - 1));
			for (size_t i = 0; i != n; i++) {
				size_t k = data[i];
				if (x)
					k = ~k;
				if ((i + 1) == n)
					k &= last_mask;
				if (k != 0)
					return false;
			}
			return true;
		}
		bool any(bool x) const { return !all(!x); }

		// Union, intersection, complement.
		//
		bool set_union(const bitset& o) {
			bool   changed = false;
			size_t l       = common_blocks(o);
			for (size_t i = 0; i != l; i++) {
				size_t a = data[i];
				size_t b = o.data[i];
				changed  = changed || ((a | b) != a);
				data[i]  = a | b;
			}
			return changed;
		}
		bool set_intersect(const bitset& o) {
			bool   changed = false;
			size_t l       = common_blocks(o);
			for (size_t i = 0; i != l; i++) {
				size_t a = data[i];
				size_t b = o.data[i];
				changed  = changed || ((a & b) != a);
				data[i]  = a & b;
			}
			return changed;
		}
		bool set_difference(const bitset& o) {
			bool   changed = false;
			size_t l       = common_blocks(o);
			for (size_t i = 0; i != l; i++) {
				size_t a = data[i];
				size_t b = o.data[i];
				changed  = changed || ((a & b) != 0);
				data[i]  = a & ~b;
			}
			return changed;
		}
		void set_complement(const bitset& o) {
			size_t l = common_blocks(o);
			for (size_t i = 0; i != l; i++) {
				data[i] = o.data[i];
			}
		}

		// Observers.
		//
		bool   empty() const { return data.empty(); }
		size_t size() const { return real_length; }
		bool   get(size_t n) const { return (data[n / width] & (1ull << (n & (width - 1)))) != 0; }

		bool set(size_t n, bool v = true) {
			size_t mask = 1ull << (n & (width - 1));
			bool   prev = (data[n / width] & mask) != 0;
			if (v)
				data[n / width] |= mask;
			else
				data[n / width] &= ~mask;
			return prev;
		}
		bool reset(size_t n) { return set(n, false); }

		size_t msb() const {
			size_t idx = data.size() * width;
			for (size_t n : view::reverse(data)) {
				int j = std::countl_zero(n);
				if (j != width) {
					return idx - j;
				}
				idx -= width;
			}
			return npos;
		}
		size_t lsb() const {
			size_t idx = 0;
			for (size_t n : data) {
				int j = std::countr_zero(n);
				if (j != width) {
					return idx + j;
				}
				idx += width;
			}
			return npos;
		}
		size_t popcount() const {
			size_t k = 0;
			for (size_t n : data) {
				k += std::popcount(n);
			}
			return k;
		}

		// Operator[].
		//
		bool operator[](size_t n) const { return get(n); }

		// Equality comparison.
		//
		bool operator==(const bitset& o) const { return real_length == o.real_length && data == o.data; }
		bool operator!=(const bitset& o) const { return !operator==(o); }
	};
};