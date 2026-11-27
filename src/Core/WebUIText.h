// WebUIText.h
// Lodestone - Shared SKSE framework
//
// The one conversion every Papyrus string goes through on its way to a page.
//
// WHY IT EXISTS. A Papyrus String holds the game's own bytes, and the game's
// text is not UTF-8: the string tables of the Western languages are Windows
// code page 1252, so "Forca" with a cedilla arrives as 46 6F 72 E7 61. Both web
// UI backends hand the bytes to their engine as UTF-8, and the engine's
// UTF-8 to UTF-16 step does not replace a bad byte - it refuses the whole
// string. Measured on the installed Prisma UI (WebCore.dll) and Magelight UI
// (MgWCore.dll): JSStringCreateWithUTF8CString answers a string of length 0 for
// "Forca" in cp1252, and for any string with a single such byte in it, while
// ASCII and real UTF-8 come through unchanged. The page then receives "" and
// draws nothing, with no error anywhere. Text that a mod wrote itself in UTF-8
// (a translation file, a JSON literal in the script) was never affected; text
// that came from the game (an item name, an NPC name, a localized label) was,
// in every language with a letter outside ASCII.
//
// THE RULE, AND WHY IT CANNOT BREAK WHAT WORKS TODAY.
//   - Only ASCII: returned as it is, without a copy. Most calls are this.
//   - Valid UTF-8: returned as it is, byte for byte. This is every string that
//     reached a page intact before this file existed, so none of them changes.
//   - Anything else: read as cp1252 and rewritten as UTF-8. Such a string
//     reached the page as "" before, so there is nothing to keep.
// Valid means strict UTF-8 (RFC 3629): no overlong forms, no surrogates,
// nothing above U+10FFFF - the same strings the engine accepts, so a string
// this file lets through as valid is one the engine would not have refused.
//
// THE ONE AMBIGUITY. A cp1252 string can happen to be valid UTF-8: an accented
// capital followed by a symbol from 0x80-0xBF, such as "A" with a tilde and a
// copyright sign. Such a string is left alone and the page shows the UTF-8
// reading of it. Real text in a Western language almost never has that pair;
// telling the two apart would need a guess about the language, and a guess
// would change strings that work today.
//
// No game header is included on purpose: the DEV bench compiles this file on
// its own, and what it tests is exactly what the DLL runs.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Lodestone::Core::WebUIText
{
	// Whether every byte is below 0x80.
	inline bool IsAscii(std::string_view a_text) noexcept
	{
		for (const char c : a_text) {
			if (static_cast<unsigned char>(c) >= 0x80) {
				return false;
			}
		}
		return true;
	}

	// Strict UTF-8, RFC 3629. ASCII is valid UTF-8.
	inline bool IsValidUtf8(std::string_view a_text) noexcept
	{
		const auto*       p   = reinterpret_cast<const unsigned char*>(a_text.data());
		const std::size_t n   = a_text.size();
		std::size_t       i   = 0;
		while (i < n) {
			const unsigned char b0 = p[i];
			if (b0 < 0x80) {
				++i;
				continue;
			}

			std::size_t   len = 0;
			unsigned char lo  = 0x80;  // allowed range of the second byte
			unsigned char hi  = 0xBF;
			if (b0 >= 0xC2 && b0 <= 0xDF) {
				len = 2;
			} else if (b0 == 0xE0) {
				len = 3;
				lo  = 0xA0;  // no overlong three-byte form
			} else if (b0 >= 0xE1 && b0 <= 0xEC) {
				len = 3;
			} else if (b0 == 0xED) {
				len = 3;
				hi  = 0x9F;  // no surrogates
			} else if (b0 >= 0xEE && b0 <= 0xEF) {
				len = 3;
			} else if (b0 == 0xF0) {
				len = 4;
				lo  = 0x90;  // no overlong four-byte form
			} else if (b0 >= 0xF1 && b0 <= 0xF3) {
				len = 4;
			} else if (b0 == 0xF4) {
				len = 4;
				hi  = 0x8F;  // nothing above U+10FFFF
			} else {
				return false;  // 0x80-0xC1 and 0xF5-0xFF never start a sequence
			}

			if (n - i < len) {
				return false;
			}
			if (p[i + 1] < lo || p[i + 1] > hi) {
				return false;
			}
			for (std::size_t k = 2; k < len; ++k) {
				if (p[i + k] < 0x80 || p[i + k] > 0xBF) {
					return false;
				}
			}
			i += len;
		}
		return true;
	}

	// The code point of one cp1252 byte at or above 0x80.
	//
	// 0x80-0x9F is where cp1252 differs from Latin-1. Its five unassigned bytes
	// (81, 8D, 8F, 90, 9D) map to the C1 control of the same number, as Windows'
	// own conversion does, so no byte is ever lost; 0xA0-0xFF are Latin-1.
	inline std::uint32_t Cp1252CodePoint(unsigned char a_byte) noexcept
	{
		static constexpr std::uint16_t kHigh[32] = {
			0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,  // 80-87
			0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,  // 88-8F
			0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,  // 90-97
			0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,  // 98-9F
		};
		if (a_byte >= 0x80 && a_byte <= 0x9F) {
			return kHigh[a_byte - 0x80];
		}
		return a_byte;
	}

	// Every byte read as cp1252, written as UTF-8.
	inline std::string Cp1252ToUtf8(std::string_view a_text)
	{
		std::string out;
		out.reserve(a_text.size() + a_text.size() / 2);
		for (const char c : a_text) {
			const std::uint32_t cp = Cp1252CodePoint(static_cast<unsigned char>(c));
			if (cp < 0x80) {
				out.push_back(static_cast<char>(cp));
			} else if (cp < 0x800) {
				out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
				out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
			} else {
				out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
				out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
			}
		}
		return out;
	}

	// The bridge's entry: makes a_text what a page can read, in place.
	//
	// Returns true when the text was converted, false when it went through as
	// it was (ASCII or valid UTF-8, untouched and not copied).
	inline bool ToPageText(std::string& a_text)
	{
		if (IsAscii(a_text) || IsValidUtf8(a_text)) {
			return false;
		}
		a_text = Cp1252ToUtf8(a_text);
		return true;
	}
}
