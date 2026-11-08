//
// mt32mem.h - the MT-32's memory for the sound-parameter protocol
//
// Golem - Copyright (C) 2026 Javier Aguilar Saavedra (shared with golem-linux)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// golem-next docs/sound-params.md, section 5: area 00 (the MT-32's memory with
// Roland's addresses), area 04 (the ROM timbres, read-only), native RQ1
// answered with DT1, and the MT-32 dump file (BLOB type 04: DT1 messages one
// after another). Munt (2.7.0) does the writing and keeps the memory; this
// file only knows Roland's map, Munt's quirks and the message formats, so the
// firmware and golemd share it and a host test can run it against a model.
//

#ifndef GOLEM_MT32MEM_H
#define GOLEM_MT32MEM_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace golem
{

// What the MT-32 engine gives this file
class MT32Access
{
public:
	virtual ~MT32Access() = default;

	// Munt's memory (Synth::readMemory) at a Munt address, with whatever the
	// music has queued applied first, so a read sees the writes before it
	virtual void ReadMunt(uint32_t nAddress, size_t nSize, uint8_t* pOut) = 0;

	// A whole DT1 message to unit 17, in order with the music, as if a song
	// had sent it
	virtual void SendDT1(const uint8_t* pMessage, size_t nSize) = 0;
};

namespace mt32mem
{

// Roland's three 7-bit bytes as one 21-bit address
constexpr uint32_t Address(uint32_t a2, uint32_t a1, uint32_t a0) { return (a2 << 14) | (a1 << 7) | a0; }

constexpr size_t TimbreSize    = 246; // bytes of a timbre; the map gives it 256 (02 00)
constexpr unsigned ROMTimbres  = 192; // area 04: A1-B64, then the rhythm timbres
constexpr size_t MaxDT1Data    = 256; // per reply, as the MT-32's DT1

// Area 00 or 04: nSize bytes from nAddress, all of them readable, or
// param::BadAddress
uint8_t Read(MT32Access& Access, uint8_t nArea, uint32_t nAddress, size_t nSize, std::vector<uint8_t>& Out);

// Area 00: one DT1 with the data. Every byte must fall in the map, or
// param::BadAddress with nBad its position, and nothing is written. Values
// over a parameter's maximum are clipped by Munt, as the MT-32 does.
uint8_t Write(MT32Access& Access, uint32_t nAddress, const uint8_t* pData, size_t nSize, size_t& nBad);

// A whole DT1 message (F0 41 10 16 12 a2 a1 a0 data cs F7)
std::vector<uint8_t> MakeDT1(uint32_t nAddress, const uint8_t* pData, size_t nSize);

// A native RQ1 to unit 17: F0 41 10 16 11 a2 a1 a0 s2 s1 s0 cs F7, checksum
// right. Requests to other units, and relative to a channel, are not answered.
bool IsRQ1(const uint8_t* pData, size_t nSize, uint32_t& nAddress, uint32_t& nLength);

// The DT1 messages that answer an RQ1: only the valid part, cut at the end of
// the zone the address is in, up to 256 data bytes each. None when the
// address is outside the map or in a zone that cannot be read (display,
// reset). The sender spaces them at least 20 ms apart.
std::vector<std::vector<uint8_t>> AnswerRQ1(MT32Access& Access, uint32_t nAddress, uint32_t nLength);

// The MT-32 dump (BLOB type 04): timbre memory (64 x 246 bytes), patch
// memory, rhythm setup and system, as DT1 messages
std::vector<uint8_t> Dump(MT32Access& Access);

// A dump in: DT1 messages to unit 17 one after another, each with its
// checksum and every byte in the map. Checked whole first; on a fault nothing
// is sent, nBad is the offset of the faulty message and the result is
// param::BadValue (format) or param::BadAddress (outside the map).
uint8_t LoadDump(MT32Access& Access, const uint8_t* pData, size_t nSize, size_t& nBad);

} // namespace mt32mem
} // namespace golem

#endif
