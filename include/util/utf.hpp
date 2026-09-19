#pragma once
#include <bit>
#include <cstring>
#include <span>
#include <string_view>
#include <util/common.hpp>

namespace li::util {
	template<typename C, bool ForeignEndianness = false>
	struct codepoint_cvt;

	// UTF-8.
	//
	template<typename T, bool ForeignEndianness>
		requires(sizeof(T) == 1)
	struct codepoint_cvt<T, ForeignEndianness> {
		//    7 bits
		// 0xxxxxxx
		//    5 bits  6 bits
		// 110xxxxx 10xxxxxx
		//    4 bits  6 bits  6 bits
		// 1110xxxx 10xxxxxx 10xxxxxx
		//    3 bits  6 bits  6 bits  6 bits
		// 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
		//
		static constexpr size_t max_out = 4;

		inline static constexpr uint8_t rlength(T _front) {
			uint8_t front  = uint8_t(_front);
			uint8_t result = (front >> 7) + 1;
			result += front >= 0b11100000;
			result += front >= 0b11110000;
			return result;
		}
		inline static constexpr uint8_t length(uint32_t cp) {
			uint8_t result = 1;
			result += bool(cp >> 7);
			result += bool(cp >> (5 + 6));
			result += bool(cp >> (4 + 6 + 6));
			return result;
		}
		inline static constexpr void encode(uint32_t cp, T*& out) {
			// Handle single character case.
			//
			if (cp <= 0x7F) [[likely]] {
				*out++ = T(cp);
				return;
			}

			auto write = [&]<auto I>(std::integral_constant<size_t, I>) LI_INLINE {
				auto p = out;
				for (size_t i = 0; i != I; i++) {
					uint8_t  flag  = i ? 0x80 : (uint8_t) fill_bits(I, 8 - I);
					uint32_t value = cp >> 6 * (I - i - 1);
					value &= fill_bits(i ? 6 : 7 - I);
					p[i] = (T) uint8_t(flag | value);
				}

				out += I;
			};

			if ((cp >> 6) <= fill_bits(5)) [[likely]]
				write(std::integral_constant<size_t, 2ull>{});
			else if ((cp >> 12) <= fill_bits(4)) [[likely]]
				write(std::integral_constant<size_t, 3ull>{});
			else
				write(std::integral_constant<size_t, 4ull>{});
		}
		inline static constexpr uint32_t decode(std::basic_string_view<T>& in) {
			T front = in[0];
			if (int8_t(front) >= 0) [[likely]] {
				in.remove_prefix(1);
				return front;
			}

			auto read = [&]<auto I>(std::integral_constant<size_t, I>) LI_INLINE -> uint32_t {
				if (in.size() < I) [[unlikely]] {
					in.remove_prefix(in.size());
					return 0;
				}
				uint32_t cp = (front & fill_bits(7 - I)) << (6 * (I - 1));
				for (size_t i = 1; i != I; i++)
					cp |= uint32_t(in[i] & fill_bits(6)) << (6 * (I - 1 - i));
				in.remove_prefix(I);
				return cp;
			};

			if (uint8_t(front) < 0b11100000) [[likely]]
				return read(std::integral_constant<size_t, 2ull>{});
			if (uint8_t(front) < 0b11110000) [[likely]]
				return read(std::integral_constant<size_t, 3ull>{});
			return read(std::integral_constant<size_t, 4ull>{});
		}
	};

	// UTF-16.
	//
	template<typename T, bool ForeignEndianness>
		requires(sizeof(T) == 2)
	struct codepoint_cvt<T, ForeignEndianness> {
		static constexpr size_t max_out = 2;

		inline static constexpr uint8_t rlength(T front) {
			if constexpr (ForeignEndianness)
				front = (T) util::bswap((uint16_t) front);
			uint8_t result = 1 + ((uint16_t(front) >> 10) == 0x36);
			return result;
		}
		inline static constexpr uint8_t length(uint32_t cp) {
			// Assuming valid codepoint outside the surrogates.
			uint8_t result = 1 + bool(cp >> 16);
			return result;
		}
		inline static constexpr void encode(uint32_t cp, T*& out) {
			T* p = out;

			// Save the old CP, determine length.
			//
			const uint16_t word_cp  = uint16_t(cp);
			const bool     has_high = cp != word_cp;
			out += has_high + 1;

			// Adjust the codepoint, encode as extended.
			//
			cp -= 0x10000;
			uint16_t lo = 0xD800 | uint16_t(cp >> 10);
			uint16_t hi = 0xDC00 | uint16_t(cp);

			// Swap the beginning with 1-byte version if not extended.
			//
			if (!has_high)
				lo = word_cp;

			// Write the data and return the size.
			//
			if constexpr (ForeignEndianness)
				lo = util::bswap(lo), hi = util::bswap(hi);
			p[has_high] = T(hi);
			p[0]        = T(lo);
		}
		inline static constexpr uint32_t decode(std::basic_string_view<T>& in) {
			// Read the low pair, rotate.
			//
			uint16_t lo = in[0];
			if constexpr (ForeignEndianness)
				lo = util::bswap(lo);
			uint16_t lo_flg = lo & fill_bits(6, 10);

			// Read the high pair.
			//
			const bool has_high = lo_flg == 0xD800 && in.size() != 1;
			uint16_t   hi       = in[has_high];
			if constexpr (ForeignEndianness)
				hi = util::bswap(hi);

			// Adjust the codepoint accordingly.
			//
			uint32_t cp = hi - 0xDC00 + 0x10000 - (0xD800 << 10);
			cp += lo << 10;
			if (!has_high)
				cp = hi;

			// Adjust the string view and return.
			//
			in.remove_prefix(has_high + 1);
			return cp;
		}
	};

	// UTF-32.
	//
	template<typename T, bool ForeignEndianness>
		requires(sizeof(T) == 4)
	struct codepoint_cvt<T, ForeignEndianness> {
		static constexpr size_t max_out = 1;

		inline static constexpr uint8_t rlength(T) { return 1; }
		inline static constexpr uint8_t length(uint32_t) { return 1; }
		inline static constexpr void    encode(uint32_t cp, T*& out) {
			if constexpr (ForeignEndianness)
				cp = util::bswap(cp);
			*out++ = (T) cp;
		}
		inline static constexpr uint32_t decode(std::basic_string_view<T>& in) {
			uint32_t cp = (uint32_t) in.front();
			in.remove_prefix(1);
			if constexpr (ForeignEndianness)
				cp = util::bswap(cp);
			return cp;
		}
	};

	template<typename To, typename From, bool Foreign = false>
	inline static std::basic_string<To> utf_convert(std::basic_string_view<From> in) {
		// Construct a view and compute the maximum length.
		//
		std::basic_string_view<From> view{in};
		size_t                       max_out = codepoint_cvt<To>::max_out * view.size();

		// Reserve the maximum length and invoke the ranged helper.
		//
		std::basic_string<To> result(max_out, '\0');

		// Re-encode every character.
		//
		To* it = result.data();
		while (!in.empty()) {
			uint32_t cp = codepoint_cvt<From, Foreign>::decode(in);
			codepoint_cvt<To>::encode(cp, it);
		}

		// Shrink the buffer and return.
		//
		result.resize(it - result.data());
		return result;
	}

	// Given a string-view, calculates the length in codepoints.
	//
	template<typename C>
	inline static size_t utf_length(std::basic_string_view<C> data) {
		size_t result = 0;
		while (!data.empty()) {
			size_t n = codepoint_cvt<C>::rlength(data.front());
			data.remove_prefix(std::min(n, data.size()));
			result++;
		}
		return result;
	}

	// Converts complete UTF-16/32 code units from an arbitrarily aligned byte
	// span. The aligned local array also gives each code unit an object lifetime.
	//
	template<typename To, typename From, bool Foreign = false>
		requires(sizeof(From) == 2 || sizeof(From) == 4)
	inline static std::basic_string<To> utf_convert_unaligned(std::span<const uint8_t> data) {
		size_t                unit_count = data.size() / sizeof(From);
		std::basic_string<To> result(codepoint_cvt<To>::max_out * unit_count, '\0');
		To*                   out = result.data();

		while (unit_count) {
			From   units[2]{};
			size_t copied = std::min(unit_count, size_t(2));
			std::memcpy(units, data.data(), copied * sizeof(From));

			std::basic_string_view<From> view{units, copied};
			size_t                       before   = view.size();
			uint32_t                     cp       = codepoint_cvt<From, Foreign>::decode(view);
			size_t                       consumed = before - view.size();
			codepoint_cvt<To>::encode(cp, out);
			data = data.subspan(consumed * sizeof(From));
			unit_count -= consumed;
		}

		result.resize(out - result.data());
		return result;
	}

	// Given a string-view, strips UTF-8 byte order mark if included, otherwise, returns true if it includes UTF-16/32 marks.
	//
	inline static bool utf_is_bom(std::string_view& data) {
		// Skip UTF-8.
		//
		if (data.size() >= 3 && !std::memcmp(data.data(), "\xEF\xBB\xBF", 3)) {
			data.remove_prefix(3);
			return false;
		}

		// Try matching against UTF-32 LE/BE:
		//
		if (data.size() >= sizeof(char32_t)) {
			char32_t front;
			std::memcpy(&front, data.data(), sizeof(front));
			if (front == 0xFEFF || front == util::bswap<char32_t>(0xFEFF))
				return true;
		}
		// Try matching against UTF-16 LE/BE:
		//
		if (data.size() >= sizeof(char16_t)) {
			char16_t front;
			std::memcpy(&front, data.data(), sizeof(front));
			if (front == 0xFEFF || front == util::bswap<char16_t>(0xFEFF))
				return true;
		}
		return false;
	}

	// Given a span of raw-data, identifies the encoding using the byte order mark
	// if relevant and re-encodes into the requested codepoint.
	//
	template<typename To>
	inline static std::basic_string<To> utf_convert(std::span<const uint8_t> data) {
		// If stream does not start with UTF8 BOM:
		//
		if (data.size() < 3 || std::memcmp(data.data(), "\xEF\xBB\xBF", 3)) {
			// Try matching against UTF-32 LE/BE:
			//
			if (data.size() >= sizeof(char32_t)) {
				char32_t front;
				std::memcpy(&front, data.data(), sizeof(front));
				if (front == 0xFEFF) [[unlikely]]
					return utf_convert_unaligned<To, char32_t, false>(data.subspan(sizeof(front)));
				if (front == util::bswap<char32_t>(0xFEFF)) [[unlikely]]
					return utf_convert_unaligned<To, char32_t, true>(data.subspan(sizeof(front)));
			}
			// Try matching against UTF-16 LE/BE:
			//
			if (data.size() >= sizeof(char16_t)) {
				char16_t front;
				std::memcpy(&front, data.data(), sizeof(front));
				if (front == 0xFEFF)
					return utf_convert_unaligned<To, char16_t, false>(data.subspan(sizeof(front)));
				if (front == util::bswap<char16_t>(0xFEFF)) [[unlikely]]
					return utf_convert_unaligned<To, char16_t, true>(data.subspan(sizeof(front)));
			}
		}
		// Otherwise remove the header and continue with the default.
		//
		else {
			data = data.subspan(3);
		}

		// Decode as UTF-8 and return.
		//
		return utf_convert<To>(std::string_view{(const char*) data.data(), data.size()});
	}
};