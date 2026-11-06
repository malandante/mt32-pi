//
// opl3work.h - the OPL3 working bank (golem-next docs/sound-params.md 6.5)
//
// Golem - Copyright (C) 2026 Javier Aguilar Saavedra
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the OPL3 engine plays, as a WOPL bank in memory that the parameter
// protocol reads and edits. It survives GM System On and the stop sweep,
// which make a new player from it; choosing a bank (SET_PARAM, .golem bank)
// replaces it. Made from the chosen bank the first time it is needed.
//
// Shared with golemd's logic and free of the kernel, so it runs in the host
// tests: COPL3Synth owns one and does the player swaps under its lock.
//

#ifndef GOLEM_OPL3WORK_H
#define GOLEM_OPL3WORK_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <vector>

#include "golem/oplbank.h"

struct ADL_MIDIPlayer;

namespace golem
{

class OPL3Work
{
public:
	// Builds a player from WOPL bytes (emulator and chips as the engine's)
	using PrepareBytesFn = std::function<ADL_MIDIPlayer*(const uint8_t* pData, size_t nSize)>;
	// Builds a player for the chosen bank (embedded or file)
	using PrepareChosenFn = std::function<ADL_MIDIPlayer*()>;
	// Reads the chosen bank's file
	using ReadFileFn = std::function<bool(std::vector<uint8_t>& data)>;

	// The chosen bank changed: the working bank is made again when needed
	void Reset();

	// The working bank, made on first use: from a player with the chosen bank
	// when it is embedded (no file to read), from the file otherwise.
	// nullptr if that fails.
	const OPLBank* Get(ADL_MIDIPlayer* pChosen, bool bEmbedded, const ReadFileFn& readFile);
	const OPLBank& Bank() const { return m_Bank; }
	bool Made() const { return m_Source != Source::None; }

	// Anything that may differ from the chosen bank: edits or an upload
	bool Changed() const { return m_bUploaded || !m_Edited.empty() || m_bHeaderEdited; }
	bool Uploaded() const { return m_bUploaded; }

	// Area 01: whole records, checked first (nothing changes if one byte is
	// refused, nBad = its position in pRecords). On OK, Live lists the
	// instruments (bank << 7 | program) to update in the playing player.
	uint8_t WriteRecords(int nFirstRecord, int nCount, const uint8_t* pRecords, size_t& nBad, std::vector<int>& Live);

	// Area 02. bNewPlayer: bank numbers or the header changed, which a
	// player hears only when built again from the bank (PrepareFrom).
	uint8_t WriteDirectory(uint32_t nAddress, const uint8_t* pData, size_t nSize, size_t& nBad, bool& bNewPlayer);

	// An uploaded bank (BLOB) replaces the working bank
	void SetUploaded(OPLBank&& bank);

	// A player that sounds like the working bank. For an embedded bank, the
	// chosen bank's player and then the edited instruments, so the rest is
	// exactly the embedded bank (its setup is not all in WOPL).
	ADL_MIDIPlayer* NewPlayer(const PrepareChosenFn& prepareChosen, const PrepareBytesFn& prepareBytes) const;
	ADL_MIDIPlayer* PrepareFrom(const OPLBank& bank, const PrepareBytesFn& prepareBytes) const;

	// One instrument of the working bank into a player
	void Apply(ADL_MIDIPlayer* pPlayer, int nKey) const;

private:
	// Where the working bank comes from: not made yet (the chosen bank as is),
	// an embedded bank plus edited instruments, or WOPL bytes (a file, an
	// upload, or an embedded bank whose header or bank numbers were edited)
	enum class Source
	{
		None,
		Embedded,
		Bytes,
	};

	Source m_Source = Source::None;
	OPLBank m_Bank;
	std::set<int> m_Edited; // records edited on an embedded bank (bank << 7 | program)
	bool m_bUploaded     = false;
	bool m_bHeaderEdited = false; // directory or header written, or records of a file bank
};

} // namespace golem

#endif
