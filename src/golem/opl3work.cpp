//
// opl3work.cpp
//
// Golem - Copyright (C) 2026 Javier Aguilar Saavedra
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "golem/opl3work.h"

#include <adlmidi.h>

namespace golem
{

void OPL3Work::Reset()
{
	m_Source = Source::None;
	m_Bank   = OPLBank();
	m_Edited.clear();
	m_bUploaded     = false;
	m_bHeaderEdited = false;
}

const OPLBank* OPL3Work::Get(ADL_MIDIPlayer* pChosen, bool bEmbedded, const ReadFileFn& readFile)
{
	if (m_Source != Source::None)
		return &m_Bank;
	if (bEmbedded)
	{
		if (!m_Bank.FromPlayer(pChosen))
			return nullptr;
		m_Source = Source::Embedded;
	}
	else
	{
		std::vector<uint8_t> Data;
		if (!readFile(Data) || !m_Bank.Load(Data.data(), Data.size()))
			return nullptr;
		m_Source = Source::Bytes;
	}
	m_Edited.clear();
	return &m_Bank;
}

uint8_t OPL3Work::WriteRecords(int nFirst, int nCount, const uint8_t* pRecords, size_t& nBad, std::vector<int>& Live)
{
	Live.clear();
	for (int i = 0; i < nCount; ++i)
	{
		const int nRecord = nFirst + i;
		if (!m_Bank.HasInstrument(nRecord >> 7, nRecord & 0x7F))
		{
			nBad = static_cast<size_t>(i) * OPLBank::RecordSize;
			return OPLBank::BadAddress;
		}
		size_t nOffset = 0;
		if (const uint8_t nError = OPLBank::ValidateRecord(pRecords + i * OPLBank::RecordSize, nOffset))
		{
			nBad = static_cast<size_t>(i) * OPLBank::RecordSize + nOffset;
			return nError;
		}
	}
	for (int i = 0; i < nCount; ++i)
	{
		const int nRecord = nFirst + i;
		size_t nUnused = 0;
		m_Bank.WriteRecord(nRecord >> 7, nRecord & 0x7F, pRecords + i * OPLBank::RecordSize, nUnused);
		if (m_Source == Source::Embedded)
			m_Edited.insert(nRecord);
		else
			m_bHeaderEdited = true;
		Live.push_back(nRecord);
	}
	return OPLBank::OK;
}

uint8_t OPL3Work::WriteDirectory(uint32_t nAddress, const uint8_t* p, size_t n, size_t& nBad, bool& bNewPlayer)
{
	bNewPlayer = false;
	// Checked on a copy, so nothing changes if one byte is refused
	OPLBank Copy;
	const std::vector<uint8_t> Saved = m_Bank.Save();
	if (!Copy.Load(Saved.data(), Saved.size()))
		return 0x06; // failed
	bool bSound = false;
	for (size_t i = 0; i < n; ++i)
	{
		const uint32_t nAddr = nAddress + static_cast<uint32_t>(i);
		if (const uint8_t nError = Copy.WriteDirectory(nAddr, p[i]))
		{
			nBad = i;
			return nError;
		}
		const uint32_t nOff = nAddr & 0x7F;
		bSound |= (nAddr >> 14) == 1 || nOff == 0x01 || nOff == 0x02;
	}
	for (size_t i = 0; i < n; ++i)
		m_Bank.WriteDirectory(nAddress + static_cast<uint32_t>(i), p[i]);
	m_bHeaderEdited = true;
	if (bSound)
	{
		// An embedded bank cannot take a new header: from now on it is WOPL
		m_Source = Source::Bytes;
		m_Edited.clear();
		bNewPlayer = true;
	}
	return OPLBank::OK;
}

void OPL3Work::SetUploaded(OPLBank&& bank)
{
	m_Bank   = std::move(bank);
	m_Source = Source::Bytes;
	m_Edited.clear();
	m_bUploaded = true;
}

ADL_MIDIPlayer* OPL3Work::PrepareFrom(const OPLBank& bank, const PrepareBytesFn& prepareBytes) const
{
	const std::vector<uint8_t> Data = bank.Save();
	return Data.empty() ? nullptr : prepareBytes(Data.data(), Data.size());
}

ADL_MIDIPlayer* OPL3Work::NewPlayer(const PrepareChosenFn& prepareChosen, const PrepareBytesFn& prepareBytes) const
{
	if (m_Source == Source::Bytes)
		return PrepareFrom(m_Bank, prepareBytes);
	ADL_MIDIPlayer* pPlayer = prepareChosen();
	if (pPlayer && m_Source == Source::Embedded)
		for (const int nKey : m_Edited)
			Apply(pPlayer, nKey);
	return pPlayer;
}

void OPL3Work::Apply(ADL_MIDIPlayer* pPlayer, int nKey) const
{
	ADL_Instrument Ins;
	ADL_BankId Id;
	ADL_Bank Bank;
	if (m_Bank.ToADL(nKey >> 7, nKey & 0x7F, &Ins, Id.percussive, Id.msb, Id.lsb) &&
	    adl_getBank(pPlayer, &Id, ADLMIDI_Bank_Create, &Bank) == 0)
		adl_setInstrument(pPlayer, &Bank, static_cast<unsigned>(nKey & 0x7F), &Ins);
}

} // namespace golem
