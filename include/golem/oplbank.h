//
// oplbank.h - the OPL3 working bank: WOPL in memory, plus the WOPLX extras
//
// Golem - Copyright (C) 2026 Javier Aguilar Saavedra (shared with golem-linux)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// golem-next docs/sound-params.md: area 01 (instruments, 128 bytes each, one
// parameter per byte), area 02 (bank directory and header) and the WOPLX
// format (section 7.1): a WOPL version 3 file followed by a "GLMX" block that
// libADLMIDI ignores. The WOPL reading and writing is libADLMIDI's own
// (src/wopl/wopl_file.c).
//

#ifndef GOLEM_OPLBANK_H
#define GOLEM_OPLBANK_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

struct WOPLFile;
struct WOPLInstrument;
struct ADL_MIDIPlayer;

namespace golem
{

class OPLBank
{
public:
	static constexpr size_t RecordSize = 128; // one instrument in area 01
	static constexpr int MaxBanks      = 64;  // melodic, and as many percussion

	// Protocol error codes (sound-params.md 3.5)
	static constexpr uint8_t OK          = 0x00;
	static constexpr uint8_t BadValue    = 0x04;
	static constexpr uint8_t BadAddress  = 0x08;

	OPLBank();
	~OPLBank();
	OPLBank(OPLBank&&) noexcept;
	OPLBank& operator=(OPLBank&&) noexcept;

	// A WOPL or WOPLX file. A damaged GLMX block is ignored (the bank still
	// loads); false only if the WOPL part does not load.
	bool Load(const uint8_t* pData, size_t nSize);

	// The banks and instruments of a running player (an embedded bank, which
	// has no file). Instrument names are empty.
	bool FromPlayer(ADL_MIDIPlayer* pPlayer);

	bool Valid() const { return m_pFile != nullptr; }

	// WOPL version 3, with the GLMX block when any instrument has extras
	std::vector<uint8_t> Save() const;

	int MelodicBanks() const;
	int PercussionBanks() const;

	// Area 01: nBank 0-63 melodic, 64-127 percussion; nProgram 0-127
	bool HasInstrument(int nBank, int nProgram) const;
	void ReadRecord(int nBank, int nProgram, uint8_t* pRecord) const;
	// Checks the whole record first: BadValue (with the offset) and nothing
	// changed if any byte is out of range
	uint8_t WriteRecord(int nBank, int nProgram, const uint8_t* pRecord, size_t& nBadOffset);
	static uint8_t ValidateRecord(const uint8_t* pRecord, size_t& nBadOffset);

	// Area 02: the directory (00 bb oo) and the header (01 00 oo). 128 bytes
	// per bank entry; nAddress is the linear address within the area.
	uint8_t ReadDirectory(uint32_t nAddress, uint8_t& nValue) const;
	uint8_t WriteDirectory(uint32_t nAddress, uint8_t nValue);

	// libADLMIDI's view of one instrument and its bank, to update a playing
	// player without a rebuild
	bool ToADL(int nBank, int nProgram, void* pADLInstrument, uint8_t& nPercussive, uint8_t& nMSB,
	           uint8_t& nLSB) const;

	struct Extras
	{
		int8_t FineTune1 = 0; // cents
		int8_t FineTune2 = 0;
		uint8_t Curve    = 0; // 0: the bank's
		bool Empty() const { return FineTune1 == 0 && FineTune2 == 0 && Curve == 0; }
	};
	Extras GetExtras(int nBank, int nProgram) const;

	// CRC-32 (IEEE), the one of the GLMX block
	static uint32_t CRC32(const uint8_t* p, size_t n);

private:
	WOPLInstrument* Instrument(int nBank, int nProgram) const;
	void LoadExtensions(const uint8_t* p, size_t n);

	struct FileDeleter
	{
		void operator()(WOPLFile* p) const;
	};
	std::unique_ptr<WOPLFile, FileDeleter> m_pFile;
	std::map<int, Extras> m_Extras; // key: bank << 7 | program
};

} // namespace golem

#endif
