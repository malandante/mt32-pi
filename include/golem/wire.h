//
// wire.h - byte encodings of the sound-parameter protocol
//
// Golem - Copyright (C) 2026 Javier Aguilar Saavedra (shared with golem-linux)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// golem-next docs/sound-params.md, section 3: Roland checksum, 14 and 21-bit
// values in 7-bit bytes, and 8-bit data packed 7 in 8.
//

#ifndef GOLEM_WIRE_H
#define GOLEM_WIRE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace golem
{
namespace wire
{

// The byte that makes the sum of the bytes plus itself a multiple of 128
inline uint8_t Checksum(const uint8_t* p, size_t n)
{
	unsigned nSum = 0;
	for (size_t i = 0; i < n; ++i)
		nSum += p[i];
	return static_cast<uint8_t>((128 - (nSum & 0x7F)) & 0x7F);
}

inline bool ChecksumOK(const uint8_t* p, size_t n) // n includes the checksum
{
	unsigned nSum = 0;
	for (size_t i = 0; i < n; ++i)
		nSum += p[i];
	return (nSum & 0x7F) == 0;
}

// 14 bits: low 7 first (as the SoundFont index of protocol v1)
inline uint16_t Get14(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 7)); }
inline void Put14(std::vector<uint8_t>& v, unsigned n)
{
	v.push_back(n & 0x7F);
	v.push_back((n >> 7) & 0x7F);
}

// 21 bits: most significant first (as Roland addresses)
inline uint32_t Get21(const uint8_t* p) { return (uint32_t(p[0]) << 14) | (uint32_t(p[1]) << 7) | p[2]; }
inline void Put21(std::vector<uint8_t>& v, uint32_t n)
{
	v.push_back((n >> 14) & 0x7F);
	v.push_back((n >> 7) & 0x7F);
	v.push_back(n & 0x7F);
}

// 7 in 8: each group of up to 7 bytes is preceded by a byte with their top
// bits (bit i = bit 7 of byte i of the group)
inline size_t PackedSize(size_t n) { return n + (n + 6) / 7; }

inline void Pack(std::vector<uint8_t>& out, const uint8_t* p, size_t n)
{
	for (size_t i = 0; i < n; i += 7)
	{
		const size_t nGroup = n - i < 7 ? n - i : 7;
		uint8_t nHigh = 0;
		for (size_t j = 0; j < nGroup; ++j)
			nHigh |= ((p[i + j] >> 7) & 1) << j;
		out.push_back(nHigh);
		for (size_t j = 0; j < nGroup; ++j)
			out.push_back(p[i + j] & 0x7F);
	}
}

// false if a group header announces bits for bytes that are not there
inline bool Unpack(std::vector<uint8_t>& out, const uint8_t* p, size_t n)
{
	size_t i = 0;
	while (i < n)
	{
		const uint8_t nHigh = p[i++];
		const size_t nGroup = n - i < 7 ? n - i : 7;
		if (nGroup == 0 || (nHigh >> nGroup) != 0)
			return false;
		for (size_t j = 0; j < nGroup; ++j)
			out.push_back(static_cast<uint8_t>(p[i + j] | (((nHigh >> j) & 1) << 7)));
		i += nGroup;
	}
	return true;
}

} // namespace wire
} // namespace golem

#endif
