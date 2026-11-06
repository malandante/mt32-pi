//
// oplbank.cpp
//
// Golem - Copyright (C) 2026 Javier Aguilar Saavedra (shared with golem-linux)
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "golem/oplbank.h"

#include <algorithm>
#include <cstring>

#include <adlmidi.h>
#include <wopl_file.h>

namespace golem
{

namespace
{
constexpr uint16_t WOPLVersion = 3;
const uint8_t ExtMagic[4]       = {'G', 'L', 'M', 'X'};
constexpr uint16_t ExtVersion   = 1;
constexpr uint8_t RecInstrument = 0x01;
constexpr uint8_t SubFine1 = 0x01, SubFine2 = 0x02, SubCurve = 0x03;

// Operator block in the record (sound-params.md 6.3)
constexpr uint8_t OpBase = 0x20, OpSize = 0x10, NameBase = 0x60, NameSize = 32;

// Highest value of each common byte (0x00-0x1F); 0 for reserved bytes
const uint8_t CommonMax[0x20] = {
	2, 1, 5, 1, 127, 127, 127, 127, 127, 7, 1, 7, 1, 127, 127, 127, // 00-0F
	127, 127, 127, 127, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,          // 10-1F
};
const uint8_t OpMax[OpSize] = {1, 1, 1, 1, 15, 3, 63, 15, 15, 15, 15, 7, 0, 0, 0, 0};

int Clamp(int v, int lo, int hi) { return std::min(std::max(v, lo), hi); }

void PutBE16(std::vector<uint8_t>& v, unsigned n)
{
	v.push_back((n >> 8) & 0xFF);
	v.push_back(n & 0xFF);
}
void PutBE32(std::vector<uint8_t>& v, uint32_t n)
{
	PutBE16(v, n >> 16);
	PutBE16(v, n & 0xFFFF);
}
uint16_t GetBE16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t GetBE32(const uint8_t* p) { return uint32_t(GetBE16(p)) << 16 | GetBE16(p + 2); }

int Key(int nBank, int nProgram) { return nBank << 7 | nProgram; }

void CopyFromADL(WOPLInstrument& w, const ADL_Instrument& a)
{
	w.note_offset1          = a.note_offset1;
	w.note_offset2          = a.note_offset2;
	w.midi_velocity_offset  = a.midi_velocity_offset;
	w.second_voice_detune   = a.second_voice_detune;
	w.percussion_key_number = a.percussion_key_number;
	w.inst_flags            = a.inst_flags;
	w.fb_conn1_C0           = a.fb_conn1_C0;
	w.fb_conn2_C0           = a.fb_conn2_C0;
	for (int i = 0; i < 4; ++i)
	{
		w.operators[i].avekf_20    = a.operators[i].avekf_20;
		w.operators[i].ksl_l_40    = a.operators[i].ksl_l_40;
		w.operators[i].atdec_60    = a.operators[i].atdec_60;
		w.operators[i].susrel_80   = a.operators[i].susrel_80;
		w.operators[i].waveform_E0 = a.operators[i].waveform_E0;
	}
	w.delay_on_ms  = a.delay_on_ms;
	w.delay_off_ms = a.delay_off_ms;
}
} // namespace

void OPLBank::FileDeleter::operator()(WOPLFile* p) const
{
	WOPL_Free(p);
}

OPLBank::OPLBank() = default;
OPLBank::~OPLBank() = default;
OPLBank::OPLBank(OPLBank&&) noexcept = default;
OPLBank& OPLBank::operator=(OPLBank&&) noexcept = default;

uint32_t OPLBank::CRC32(const uint8_t* p, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; ++i)
	{
		c ^= p[i];
		for (int k = 0; k < 8; ++k)
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
	}
	return ~c;
}

bool OPLBank::Load(const uint8_t* pData, size_t nSize)
{
	std::vector<uint8_t> Copy(pData, pData + nSize); // the reader takes a non-const pointer
	int nError = 0;
	WOPLFile* pFile = WOPL_LoadBankFromMem(Copy.data(), Copy.size(), &nError);
	if (!pFile)
		return false;
	if (pFile->banks_count_melodic > MaxBanks || pFile->banks_count_percussion > MaxBanks)
	{
		WOPL_Free(pFile);
		return false;
	}
	m_pFile.reset(pFile);
	m_Extras.clear();

	const size_t nBase = WOPL_CalculateBankFileSize(pFile, pFile->version);
	if (nBase < nSize)
		LoadExtensions(pData + nBase, nSize - nBase);
	return true;
}

void OPLBank::LoadExtensions(const uint8_t* p, size_t n)
{
	// magic 4, version 2, length 4, records, CRC 4
	if (n < 14 || memcmp(p, ExtMagic, 4) != 0 || GetBE16(p + 4) != ExtVersion)
		return;
	const uint32_t nLength = GetBE32(p + 6);
	if (nLength > n - 14)
		return;
	if (CRC32(p, 10 + nLength) != GetBE32(p + 10 + nLength))
		return;

	const uint8_t* r = p + 10;
	const uint8_t* const pEnd = r + nLength;
	std::map<int, Extras> Found;
	while (pEnd - r >= 3)
	{
		const uint8_t nType   = r[0];
		const uint16_t nSize  = GetBE16(r + 1);
		const uint8_t* pBody  = r + 3;
		if (nSize > pEnd - pBody)
			return; // truncated: keep nothing from a damaged block
		r = pBody + nSize;
		if (nType != RecInstrument || nSize < 4)
			continue; // unknown records are skipped

		const int nKind = pBody[0];
		const int nIndex = GetBE16(pBody + 1);
		const int nProgram = pBody[3];
		if (nKind > 1 || nIndex >= MaxBanks || nProgram > 127)
			continue;
		Extras& e = Found[Key(nKind * MaxBanks + nIndex, nProgram)];
		const uint8_t* s = pBody + 4;
		const uint8_t* const sEnd = pBody + nSize;
		while (sEnd - s >= 2)
		{
			const uint8_t nId = s[0], nLen = s[1];
			if (nLen > sEnd - s - 2)
				break;
			if (nLen == 1)
			{
				if (nId == SubFine1)
					e.FineTune1 = static_cast<int8_t>(s[2]);
				else if (nId == SubFine2)
					e.FineTune2 = static_cast<int8_t>(s[2]);
				else if (nId == SubCurve)
					e.Curve = s[2] & 0x7F;
			}
			s += 2 + nLen;
		}
	}
	m_Extras = std::move(Found);
}

bool OPLBank::FromPlayer(ADL_MIDIPlayer* pPlayer)
{
	if (!pPlayer)
		return false;

	struct Entry
	{
		ADL_Bank Bank;
		ADL_BankId Id;
	};
	std::vector<Entry> Melodic, Percussion;
	ADL_Bank Bank;
	if (adl_getFirstBank(pPlayer, &Bank) == 0)
	{
		do
		{
			Entry e{Bank, {}};
			if (adl_getBankId(pPlayer, &Bank, &e.Id) != 0)
				continue;
			auto& List = e.Id.percussive ? Percussion : Melodic;
			if (List.size() < static_cast<size_t>(MaxBanks))
				List.push_back(e);
		} while (adl_getNextBank(pPlayer, &Bank) == 0);
	}
	if (Melodic.empty() && Percussion.empty())
		return false;
	// libADLMIDI keeps its banks in a hash map: sorted here by bank number,
	// so bank 0 of the area is MSB 0 / LSB 0, the General MIDI one
	auto ByNumber = [](const Entry& a, const Entry& b) {
		return (a.Id.msb << 7 | a.Id.lsb) < (b.Id.msb << 7 | b.Id.lsb);
	};
	std::sort(Melodic.begin(), Melodic.end(), ByNumber);
	std::sort(Percussion.begin(), Percussion.end(), ByNumber);

	WOPLFile* pFile = WOPL_Init(static_cast<uint16_t>(std::max<size_t>(Melodic.size(), 1)),
	                            static_cast<uint16_t>(std::max<size_t>(Percussion.size(), 1)));
	if (!pFile)
		return false;
	pFile->version = WOPLVersion;
	pFile->opl_flags = (adl_getHTremolo(pPlayer) > 0 ? WOPL_FLAG_DEEP_TREMOLO : 0) |
	                   (adl_getHVibrato(pPlayer) > 0 ? WOPL_FLAG_DEEP_VIBRATO : 0);
	// libADLMIDI counts its volume models from "automatic" (0); WOPL from
	// "generic"
	const int nModel = adl_getVolumeRangeModel(pPlayer);
	pFile->volume_model = static_cast<uint8_t>(nModel > 0 ? nModel - 1 : 0);

	auto Fill = [pPlayer](WOPLBank* pBanks, std::vector<Entry>& List) {
		for (size_t i = 0; i < List.size(); ++i)
		{
			WOPLBank& b = pBanks[i];
			b.bank_midi_msb = List[i].Id.msb;
			b.bank_midi_lsb = List[i].Id.lsb;
			for (unsigned k = 0; k < 128; ++k)
			{
				ADL_Instrument a;
				memset(&a, 0, sizeof(a));
				if (adl_getInstrument(pPlayer, &List[i].Bank, k, &a) == 0)
					CopyFromADL(b.ins[k], a);
			}
		}
	};
	Fill(pFile->banks_melodic, Melodic);
	Fill(pFile->banks_percussive, Percussion);
	m_pFile.reset(pFile);
	m_Extras.clear();
	return true;
}

std::vector<uint8_t> OPLBank::Save() const
{
	std::vector<uint8_t> Out;
	if (!m_pFile)
		return Out;
	const size_t nSize = WOPL_CalculateBankFileSize(m_pFile.get(), WOPLVersion);
	Out.resize(nSize);
	if (WOPL_SaveBankToMem(m_pFile.get(), Out.data(), Out.size(), WOPLVersion, 0) != 0)
		return {};

	std::vector<uint8_t> Records;
	for (const auto& Entry : m_Extras)
	{
		const int nKey = Entry.first;
		const auto& e  = Entry.second;
		if (e.Empty())
			continue;
		const int nBank = nKey >> 7, nProgram = nKey & 0x7F;
		std::vector<uint8_t> Body = {static_cast<uint8_t>(nBank >= MaxBanks ? 1 : 0)};
		PutBE16(Body, static_cast<unsigned>(nBank % MaxBanks));
		Body.push_back(static_cast<uint8_t>(nProgram));
		if (e.FineTune1)
			Body.insert(Body.end(), {SubFine1, 1, static_cast<uint8_t>(e.FineTune1)});
		if (e.FineTune2)
			Body.insert(Body.end(), {SubFine2, 1, static_cast<uint8_t>(e.FineTune2)});
		if (e.Curve)
			Body.insert(Body.end(), {SubCurve, 1, e.Curve});
		Records.push_back(RecInstrument);
		PutBE16(Records, static_cast<unsigned>(Body.size()));
		Records.insert(Records.end(), Body.begin(), Body.end());
	}
	if (!Records.empty())
	{
		const size_t nStart = Out.size();
		Out.insert(Out.end(), ExtMagic, ExtMagic + 4);
		PutBE16(Out, ExtVersion);
		PutBE32(Out, static_cast<uint32_t>(Records.size()));
		Out.insert(Out.end(), Records.begin(), Records.end());
		PutBE32(Out, CRC32(Out.data() + nStart, Out.size() - nStart));
	}
	return Out;
}

int OPLBank::MelodicBanks() const { return m_pFile ? m_pFile->banks_count_melodic : 0; }
int OPLBank::PercussionBanks() const { return m_pFile ? m_pFile->banks_count_percussion : 0; }

WOPLInstrument* OPLBank::Instrument(int nBank, int nProgram) const
{
	if (!m_pFile || nProgram < 0 || nProgram > 127 || nBank < 0 || nBank >= 2 * MaxBanks)
		return nullptr;
	if (nBank < MaxBanks)
		return nBank < m_pFile->banks_count_melodic ? &m_pFile->banks_melodic[nBank].ins[nProgram] : nullptr;
	nBank -= MaxBanks;
	return nBank < m_pFile->banks_count_percussion ? &m_pFile->banks_percussive[nBank].ins[nProgram] : nullptr;
}

bool OPLBank::HasInstrument(int nBank, int nProgram) const { return Instrument(nBank, nProgram) != nullptr; }

OPLBank::Extras OPLBank::GetExtras(int nBank, int nProgram) const
{
	const auto it = m_Extras.find(Key(nBank, nProgram));
	return it == m_Extras.end() ? Extras{} : it->second;
}

void OPLBank::ReadRecord(int nBank, int nProgram, uint8_t* r) const
{
	memset(r, 0, RecordSize);
	const WOPLInstrument* w = Instrument(nBank, nProgram);
	if (!w)
		return;
	const uint8_t f = w->inst_flags;
	r[0x00] = (f & WOPL_Ins_4op) ? ((f & WOPL_Ins_Pseudo4op) ? 2 : 1) : 0;
	r[0x01] = (f & WOPL_Ins_IsBlank) ? 1 : 0;
	r[0x02] = (f & WOPL_RhythmModeMask) >> 3;
	r[0x03] = (f & WOPL_Ins_FixedNote) ? 1 : 0;
	r[0x04] = static_cast<uint8_t>(std::min<int>(w->percussion_key_number, 127));
	r[0x05] = static_cast<uint8_t>(Clamp(w->note_offset1, -64, 63) + 64);
	r[0x06] = static_cast<uint8_t>(Clamp(w->note_offset2, -64, 63) + 64);
	r[0x07] = static_cast<uint8_t>(Clamp(w->midi_velocity_offset, -64, 63) + 64);
	r[0x08] = static_cast<uint8_t>(Clamp(w->second_voice_detune, -64, 63) + 64);
	r[0x09] = (w->fb_conn1_C0 >> 1) & 7;
	r[0x0A] = w->fb_conn1_C0 & 1;
	r[0x0B] = (w->fb_conn2_C0 >> 1) & 7;
	r[0x0C] = w->fb_conn2_C0 & 1;
	const unsigned nOn = std::min<unsigned>(w->delay_on_ms, 16383), nOff = std::min<unsigned>(w->delay_off_ms, 16383);
	r[0x0D] = nOn & 0x7F;
	r[0x0E] = (nOn >> 7) & 0x7F;
	r[0x0F] = nOff & 0x7F;
	r[0x10] = (nOff >> 7) & 0x7F;
	const Extras e = GetExtras(nBank, nProgram);
	r[0x11] = static_cast<uint8_t>(Clamp(e.FineTune1, -64, 63) + 64);
	r[0x12] = static_cast<uint8_t>(Clamp(e.FineTune2, -64, 63) + 64);
	r[0x13] = e.Curve;
	for (int i = 0; i < 4; ++i)
	{
		const auto& o = w->operators[i];
		uint8_t* b = r + OpBase + i * OpSize;
		b[0x0] = (o.avekf_20 >> 7) & 1;
		b[0x1] = (o.avekf_20 >> 6) & 1;
		b[0x2] = (o.avekf_20 >> 5) & 1;
		b[0x3] = (o.avekf_20 >> 4) & 1;
		b[0x4] = o.avekf_20 & 0x0F;
		b[0x5] = (o.ksl_l_40 >> 6) & 3;
		b[0x6] = o.ksl_l_40 & 0x3F;
		b[0x7] = (o.atdec_60 >> 4) & 0x0F;
		b[0x8] = o.atdec_60 & 0x0F;
		b[0x9] = (o.susrel_80 >> 4) & 0x0F;
		b[0xA] = o.susrel_80 & 0x0F;
		b[0xB] = o.waveform_E0 & 7;
	}
	for (int i = 0; i < NameSize && w->inst_name[i]; ++i)
		r[NameBase + i] = (w->inst_name[i] >= 0x20 && w->inst_name[i] < 0x7F) ? w->inst_name[i] : '?';
}

uint8_t OPLBank::ValidateRecord(const uint8_t* r, size_t& nBad)
{
	for (size_t i = 0; i < RecordSize; ++i)
	{
		uint8_t nMax;
		if (i < OpBase)
			nMax = CommonMax[i];
		else if (i < NameBase)
			nMax = OpMax[(i - OpBase) % OpSize];
		else
			nMax = 0x7E; // ASCII, or 0
		const bool bName = i >= NameBase;
		if (r[i] > nMax || (bName && r[i] != 0 && r[i] < 0x20))
		{
			nBad = i;
			return BadValue;
		}
	}
	return OK;
}

uint8_t OPLBank::WriteRecord(int nBank, int nProgram, const uint8_t* r, size_t& nBad)
{
	WOPLInstrument* w = Instrument(nBank, nProgram);
	if (!w)
		return BadAddress;
	if (const uint8_t nError = ValidateRecord(r, nBad))
		return nError;

	uint8_t f = w->inst_flags & ~(WOPL_Ins_4op | WOPL_Ins_Pseudo4op | WOPL_Ins_IsBlank | WOPL_RhythmModeMask |
	                              WOPL_Ins_FixedNote);
	if (r[0x00] >= 1)
		f |= WOPL_Ins_4op;
	if (r[0x00] == 2)
		f |= WOPL_Ins_Pseudo4op;
	if (r[0x01])
		f |= WOPL_Ins_IsBlank;
	f |= (r[0x02] << 3) & WOPL_RhythmModeMask;
	if (r[0x03])
		f |= WOPL_Ins_FixedNote;
	w->inst_flags            = f;
	w->percussion_key_number = r[0x04];
	w->note_offset1          = static_cast<int16_t>(r[0x05] - 64);
	w->note_offset2          = static_cast<int16_t>(r[0x06] - 64);
	w->midi_velocity_offset  = static_cast<int8_t>(r[0x07] - 64);
	w->second_voice_detune   = static_cast<int8_t>(r[0x08] - 64);
	// Only feedback and connection: libADLMIDI sets the panning bits itself
	w->fb_conn1_C0 = static_cast<uint8_t>((w->fb_conn1_C0 & 0xF0) | (r[0x09] << 1) | r[0x0A]);
	w->fb_conn2_C0 = static_cast<uint8_t>((w->fb_conn2_C0 & 0xF0) | (r[0x0B] << 1) | r[0x0C]);
	w->delay_on_ms  = static_cast<uint16_t>(r[0x0D] | r[0x0E] << 7);
	w->delay_off_ms = static_cast<uint16_t>(r[0x0F] | r[0x10] << 7);

	Extras e;
	e.FineTune1 = static_cast<int8_t>(r[0x11] - 64);
	e.FineTune2 = static_cast<int8_t>(r[0x12] - 64);
	e.Curve     = r[0x13];
	if (e.Empty())
		m_Extras.erase(Key(nBank, nProgram));
	else
		m_Extras[Key(nBank, nProgram)] = e;

	for (int i = 0; i < 4; ++i)
	{
		auto& o = w->operators[i];
		const uint8_t* b = r + OpBase + i * OpSize;
		o.avekf_20    = static_cast<uint8_t>(b[0] << 7 | b[1] << 6 | b[2] << 5 | b[3] << 4 | b[4]);
		o.ksl_l_40    = static_cast<uint8_t>(b[5] << 6 | b[6]);
		o.atdec_60    = static_cast<uint8_t>(b[7] << 4 | b[8]);
		o.susrel_80   = static_cast<uint8_t>(b[9] << 4 | b[0xA]);
		o.waveform_E0 = static_cast<uint8_t>((o.waveform_E0 & 0xF8) | b[0xB]);
	}
	memset(w->inst_name, 0, sizeof(w->inst_name));
	for (int i = 0; i < NameSize && r[NameBase + i]; ++i)
		w->inst_name[i] = static_cast<char>(r[NameBase + i]);
	return OK;
}

uint8_t OPLBank::ReadDirectory(uint32_t nAddress, uint8_t& v) const
{
	if (!m_pFile)
		return BadAddress;
	const uint32_t nSection = nAddress >> 14, nBank = (nAddress >> 7) & 0x7F, nOff = nAddress & 0x7F;
	v = 0;
	if (nSection == 0)
	{
		const bool bPerc = nBank >= static_cast<uint32_t>(MaxBanks);
		const uint32_t nIndex = nBank % MaxBanks;
		const int nCount = bPerc ? m_pFile->banks_count_percussion : m_pFile->banks_count_melodic;
		const WOPLBank* b = static_cast<int>(nIndex) < nCount
		                        ? &(bPerc ? m_pFile->banks_percussive : m_pFile->banks_melodic)[nIndex]
		                        : nullptr;
		if (nOff == 0x00)
			v = b ? 1 : 0;
		else if (!b)
			v = 0;
		else if (nOff == 0x01)
			v = b->bank_midi_msb & 0x7F;
		else if (nOff == 0x02)
			v = b->bank_midi_lsb & 0x7F;
		else if (nOff >= 0x10 && nOff < 0x30)
		{
			const char c = b->bank_name[nOff - 0x10];
			v = (c >= 0x20 && c < 0x7F) ? static_cast<uint8_t>(c) : 0;
		}
		return OK;
	}
	if (nSection == 1 && nBank == 0)
	{
		switch (nOff)
		{
			case 0x00: v = (m_pFile->opl_flags & WOPL_FLAG_DEEP_TREMOLO) ? 1 : 0; return OK;
			case 0x01: v = (m_pFile->opl_flags & WOPL_FLAG_DEEP_VIBRATO) ? 1 : 0; return OK;
			case 0x02: v = m_pFile->volume_model & 0x7F; return OK;
			case 0x03: v = static_cast<uint8_t>(m_pFile->banks_count_melodic); return OK;
			case 0x04: v = static_cast<uint8_t>(m_pFile->banks_count_percussion); return OK;
			default: return OK; // reserved: reads as 0
		}
	}
	return BadAddress;
}

uint8_t OPLBank::WriteDirectory(uint32_t nAddress, uint8_t v)
{
	if (!m_pFile)
		return BadAddress;
	const uint32_t nSection = nAddress >> 14, nBank = (nAddress >> 7) & 0x7F, nOff = nAddress & 0x7F;
	if (nSection == 0)
	{
		const bool bPerc = nBank >= static_cast<uint32_t>(MaxBanks);
		const uint32_t nIndex = nBank % MaxBanks;
		const int nCount = bPerc ? m_pFile->banks_count_percussion : m_pFile->banks_count_melodic;
		if (static_cast<int>(nIndex) >= nCount)
			return BadAddress;
		WOPLBank& b = (bPerc ? m_pFile->banks_percussive : m_pFile->banks_melodic)[nIndex];
		if (nOff == 0x01)
			b.bank_midi_msb = v;
		else if (nOff == 0x02)
			b.bank_midi_lsb = v;
		else if (nOff >= 0x10 && nOff < 0x30)
		{
			if (v != 0 && (v < 0x20 || v > 0x7E))
				return BadValue;
			b.bank_name[nOff - 0x10] = static_cast<char>(v);
		}
		else
			return BadAddress; // presence and reserved bytes
		return OK;
	}
	if (nSection == 1 && nBank == 0)
	{
		switch (nOff)
		{
			case 0x00:
			case 0x01:
			{
				if (v > 1)
					return BadValue;
				const uint8_t nFlag = nOff == 0 ? WOPL_FLAG_DEEP_TREMOLO : WOPL_FLAG_DEEP_VIBRATO;
				m_pFile->opl_flags = static_cast<uint8_t>(v ? (m_pFile->opl_flags | nFlag) : (m_pFile->opl_flags & ~nFlag));
				return OK;
			}
			case 0x02:
				if (v > WOPL_VM_HMI)
					return BadValue;
				m_pFile->volume_model = v;
				return OK;
			default: return BadAddress;
		}
	}
	return BadAddress;
}

bool OPLBank::ToADL(int nBank, int nProgram, void* pOut, uint8_t& nPercussive, uint8_t& nMSB, uint8_t& nLSB) const
{
	const WOPLInstrument* w = Instrument(nBank, nProgram);
	if (!w)
		return false;
	ADL_Instrument& a = *static_cast<ADL_Instrument*>(pOut);
	memset(&a, 0, sizeof(a));
	a.version               = ADLMIDI_InstrumentVersion;
	a.note_offset1          = w->note_offset1;
	a.note_offset2          = w->note_offset2;
	a.midi_velocity_offset  = w->midi_velocity_offset;
	a.second_voice_detune   = w->second_voice_detune;
	a.percussion_key_number = w->percussion_key_number;
	a.inst_flags            = w->inst_flags;
	a.fb_conn1_C0           = w->fb_conn1_C0;
	a.fb_conn2_C0           = w->fb_conn2_C0;
	for (int i = 0; i < 4; ++i)
	{
		a.operators[i].avekf_20    = w->operators[i].avekf_20;
		a.operators[i].ksl_l_40    = w->operators[i].ksl_l_40;
		a.operators[i].atdec_60    = w->operators[i].atdec_60;
		a.operators[i].susrel_80   = w->operators[i].susrel_80;
		a.operators[i].waveform_E0 = w->operators[i].waveform_E0;
	}
	a.delay_on_ms  = w->delay_on_ms;
	a.delay_off_ms = w->delay_off_ms;

	const bool bPerc = nBank >= MaxBanks;
	const WOPLBank& b = (bPerc ? m_pFile->banks_percussive : m_pFile->banks_melodic)[nBank % MaxBanks];
	nPercussive = bPerc ? 1 : 0;
	nMSB = b.bank_midi_msb;
	nLSB = b.bank_midi_lsb;
	return true;
}

} // namespace golem
