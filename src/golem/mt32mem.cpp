//
// mt32mem.cpp
//
// Golem - Copyright (C) 2026 Javier Aguilar Saavedra (shared with golem-linux)
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "golem/mt32mem.h"

#include <algorithm>

#include "golem/params.h"
#include "golem/wire.h"

namespace golem
{
namespace mt32mem
{

namespace
{
// Roland's map as Munt 2.7.0 keeps it (MemoryRegion.h). MuntStart is where
// Munt reads the zone: the timbre memory is written at 08 00 00, which Munt
// moves past the 128 ROM timbres, but readMemory does not, so it is read at
// 0A 00 00 (sound-params.md 1.6).
struct Zone
{
	uint32_t Start, Size, MuntStart;
	bool Readable;
};

constexpr uint32_t TimbresStart = Address(0x08, 0, 0);
constexpr uint32_t TimbreStride = 0x100; // 02 00

constexpr Zone Zones[] = {
	{Address(0x03, 0x00, 0x00), 9 * 16, Address(0x03, 0x00, 0x00), true},     // patch temporary, parts 1-8 and rhythm
	{Address(0x03, 0x01, 0x10), 85 * 4, Address(0x03, 0x01, 0x10), true},     // rhythm setup temporary
	{Address(0x04, 0x00, 0x00), 8 * TimbreSize, Address(0x04, 0x00, 0x00), true}, // timbre temporary
	{Address(0x05, 0x00, 0x00), 128 * 8, Address(0x05, 0x00, 0x00), true},    // patch memory
	{TimbresStart, 64 * TimbreStride, TimbresStart + 128 * TimbreStride, true}, // timbre memory
	{Address(0x10, 0x00, 0x00), 23, Address(0x10, 0x00, 0x00), true},          // system
	{Address(0x20, 0x00, 0x00), 0x4013, Address(0x20, 0x00, 0x00), false},     // display
	{Address(0x7F, 0x00, 0x00), 0x3FFF, Address(0x7F, 0x00, 0x00), false},     // reset
};

const Zone* FindZone(uint32_t nAddress)
{
	for (const Zone& z : Zones)
		if (nAddress >= z.Start && nAddress < z.Start + z.Size)
			return &z;
	return nullptr;
}

// Area 04, timbre n (0-191) at n x 02 00: A1-B64 are Munt's timbres 0-127,
// the rhythm ones 192-255 (128-191 are the timbre memory)
uint32_t ROMTimbreMunt(uint32_t nAddress)
{
	const uint32_t n = nAddress / TimbreStride;
	return TimbresStart + (n < 128 ? n : n + 64) * TimbreStride + nAddress % TimbreStride;
}

void PutDT1(std::vector<uint8_t>& Out, uint32_t nAddress, const uint8_t* pData, size_t nSize)
{
	const size_t nStart = Out.size();
	Out.insert(Out.end(), {0xF0, 0x41, 0x10, 0x16, 0x12});
	wire::Put21(Out, nAddress);
	Out.insert(Out.end(), pData, pData + nSize);
	Out.push_back(wire::Checksum(Out.data() + nStart + 5, Out.size() - nStart - 5));
	Out.push_back(0xF7);
}

// Every byte of [nAddress, nAddress + nSize) in the map: the offset of the
// first that is not, or nSize
size_t FirstOutside(uint32_t nAddress, size_t nSize)
{
	size_t i = 0;
	while (i < nSize)
	{
		const Zone* pZone = FindZone(nAddress + static_cast<uint32_t>(i));
		if (!pZone)
			return i;
		i += pZone->Start + pZone->Size - (nAddress + static_cast<uint32_t>(i));
	}
	return nSize;
}
} // namespace

uint8_t Read(MT32Access& Access, uint8_t nArea, uint32_t nAddress, size_t nSize, std::vector<uint8_t>& Out)
{
	Out.clear();
	if (nArea == param::MT32ROM)
	{
		if (nAddress + nSize > ROMTimbres * TimbreStride)
			return param::BadAddress;
		// One timbre at a time: 127 and 128 are not next to each other in Munt
		while (Out.size() < nSize)
		{
			const uint32_t a  = nAddress + static_cast<uint32_t>(Out.size());
			const size_t nRun = std::min<size_t>(nSize - Out.size(), TimbreStride - a % TimbreStride);
			const size_t nAt  = Out.size();
			Out.resize(nAt + nRun);
			Access.ReadMunt(ROMTimbreMunt(a), nRun, Out.data() + nAt);
		}
		return param::OK;
	}
	if (nArea != param::MT32)
		return param::BadAddress;

	// Zone by zone: Munt reads one zone per call
	while (Out.size() < nSize)
	{
		const uint32_t a     = nAddress + static_cast<uint32_t>(Out.size());
		const Zone* const pZ = FindZone(a);
		if (!pZ || !pZ->Readable)
		{
			Out.clear();
			return param::BadAddress;
		}
		const size_t nRun = std::min<size_t>(nSize - Out.size(), pZ->Start + pZ->Size - a);
		const size_t nAt  = Out.size();
		Out.resize(nAt + nRun);
		Access.ReadMunt(pZ->MuntStart + (a - pZ->Start), nRun, Out.data() + nAt);
	}
	return param::OK;
}

uint8_t Write(MT32Access& Access, uint32_t nAddress, const uint8_t* pData, size_t nSize, size_t& nBad)
{
	const size_t nOutside = FirstOutside(nAddress, nSize);
	if (nOutside < nSize)
	{
		nBad = nOutside;
		return param::BadAddress;
	}
	const std::vector<uint8_t> Message = MakeDT1(nAddress, pData, nSize);
	Access.SendDT1(Message.data(), Message.size());
	return param::OK;
}

std::vector<uint8_t> MakeDT1(uint32_t nAddress, const uint8_t* pData, size_t nSize)
{
	std::vector<uint8_t> Out;
	PutDT1(Out, nAddress, pData, nSize);
	return Out;
}

bool IsRQ1(const uint8_t* p, size_t n, uint32_t& nAddress, uint32_t& nLength)
{
	if (n != 13 || p[0] != 0xF0 || p[1] != 0x41 || p[2] != 0x10 || p[3] != 0x16 || p[4] != 0x11 || p[12] != 0xF7)
		return false;
	for (size_t i = 5; i < 12; ++i)
		if (p[i] & 0x80)
			return false;
	if (!wire::ChecksumOK(p + 5, 7))
		return false;
	nAddress = wire::Get21(p + 5);
	nLength  = wire::Get21(p + 8);
	return true;
}

std::vector<std::vector<uint8_t>> AnswerRQ1(MT32Access& Access, uint32_t nAddress, uint32_t nLength)
{
	std::vector<std::vector<uint8_t>> Replies;
	const Zone* const pZone = FindZone(nAddress);
	if (!pZone || !pZone->Readable || nLength == 0)
		return Replies;
	if (nLength > pZone->Start + pZone->Size - nAddress)
		nLength = pZone->Start + pZone->Size - nAddress;

	std::vector<uint8_t> Data;
	if (Read(Access, param::MT32, nAddress, nLength, Data) != param::OK)
		return Replies;
	for (size_t i = 0; i < Data.size(); i += MaxDT1Data)
	{
		const size_t nChunk = std::min(MaxDT1Data, Data.size() - i);
		Replies.push_back(MakeDT1(nAddress + static_cast<uint32_t>(i), Data.data() + i, nChunk));
	}
	return Replies;
}

std::vector<uint8_t> Dump(MT32Access& Access)
{
	std::vector<uint8_t> File, Data;

	// Timbres first, so the patches and the rhythm setup that point at them
	// find them in place on the way back in
	for (uint32_t t = 0; t < 64; ++t)
	{
		const uint32_t a = TimbresStart + t * TimbreStride;
		Read(Access, param::MT32, a, TimbreSize, Data);
		PutDT1(File, a, Data.data(), Data.size());
	}

	const struct
	{
		uint32_t Address;
		size_t Size;
	} Parts[] = {
		{Address(0x05, 0x00, 0x00), 128 * 8}, // patch memory
		{Address(0x03, 0x01, 0x10), 85 * 4},  // rhythm setup
		{Address(0x10, 0x00, 0x00), 23},      // system
	};
	for (const auto& Part : Parts)
	{
		Read(Access, param::MT32, Part.Address, Part.Size, Data);
		for (size_t i = 0; i < Data.size(); i += MaxDT1Data)
			PutDT1(File, Part.Address + static_cast<uint32_t>(i), Data.data() + i,
			       std::min(MaxDT1Data, Data.size() - i));
	}
	return File;
}

uint8_t LoadDump(MT32Access& Access, const uint8_t* p, size_t n, size_t& nBad)
{
	struct Message
	{
		size_t Offset, Size;
	};
	std::vector<Message> Messages;

	size_t i = 0;
	while (i < n)
	{
		nBad = i;
		size_t j = i + 1;
		while (j < n && p[j] != 0xF7 && !(p[j] & 0x80))
			++j;
		// F0 41 10 16 12 a2 a1 a0, at least one byte of data, cs, F7
		if (p[i] != 0xF0 || j >= n || p[j] != 0xF7 || j - i + 1 < 11 || p[i + 1] != 0x41 || p[i + 2] != 0x10 ||
		    p[i + 3] != 0x16 || p[i + 4] != 0x12 || !wire::ChecksumOK(p + i + 5, j - i - 5))
			return param::BadValue;
		const uint32_t nAddress = wire::Get21(p + i + 5);
		const size_t nData      = j - i - 9;
		if (FirstOutside(nAddress, nData) < nData)
			return param::BadAddress;
		Messages.push_back({i, j - i + 1});
		i = j + 1;
	}
	nBad = 0;
	if (Messages.empty())
		return param::BadValue;

	for (const Message& m : Messages)
		Access.SendDT1(p + m.Offset, m.Size);
	return param::OK;
}

} // namespace mt32mem
} // namespace golem
