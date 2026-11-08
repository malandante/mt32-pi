//
// mt32pi_params.cpp
//
// The firmware's side of the Golem sound-parameter protocol (golem-next
// docs/sound-params.md), the same as golemd's (golem-linux
// src/engine_params.cpp). Implemented: area 00 (the MT-32's memory), 01
// (OPL3 instruments), 02 (OPL3 bank directory and header), 04 (the MT-32's
// ROM timbres), 10 sections 00 (general, with the SET_PARAMETER settings), 01
// (MT-32 engine), 02 (FluidSynth effects) and 03 (OPL3 engine), BLOB with
// WOPL/WOPLX banks, FluidSynth effects profiles and MT-32 dumps, and native
// RQ1. Other areas and addresses answer "address" (08)
// until they exist.
//
// This file is part of mt32-pi.
//
// mt32-pi is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// mt32-pi is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along with
// mt32-pi. If not, see <http://www.gnu.org/licenses/>.
//

#include <circle/logger.h>
#include <circle/timer.h>
#include <cstdio>
#include <cstring>

#include "golem/oplbank.h"
#include "mt32pi.h"

#ifndef MT32_PI_VERSION
#define MT32_PI_VERSION "<unknown>"
#endif

LOGMODULE("golemparams");

namespace
{
constexpr u32 Section(u32 nAddress) { return nAddress >> 14; }

// Area 10 parameters (sound-params.md 8.1, 8.3)
constexpr u32 SetParameterLast = 0x06; // SET_PARAMETER 00-06: volume ... loudness
constexpr u32 GeneralSynth = 0x20, GeneralROM = 0x21, GeneralSFLow = 0x22, GeneralSFHigh = 0x23,
              GeneralUSBOut = 0x24, GeneralUSBCount = 0x25, GeneralEngines = 0x26;
constexpr u32 OPL3Emulator = 0x00, OPL3Chips = 0x01, OPL3GainLow = 0x03, OPL3GainHigh = 0x04;
constexpr unsigned OPL3GainMax = 1600; // x100: 16.0
constexpr u32 MT32GainLow = 0x00, MT32GainHigh = 0x01, MT32ReverbGainLow = 0x02, MT32ReverbGainHigh = 0x03,
              MT32Resampler = 0x04, MT32Reversed = 0x05, MT32Reverb = 0x06;
constexpr unsigned MT32GainMax = 1600; // x100: 16.0

// Native RQ1 answers: no more than a whole timbre memory waiting (64 DT1)
// plus room for a second request
constexpr size_t RQ1QueueMax = 160;
constexpr unsigned RQ1GapMicros = 20000; // between DT1 messages, as the MT-32 asks
constexpr unsigned MicrosPerByte = 320;  // 10 bits at 31250 baud

unsigned Hundredths(float n) { return static_cast<unsigned>(n * 100.0f + 0.5f); }

// A whole frame or nothing: a frame cut by a full transmit buffer would
// corrupt the Next's MIDI stream. At 31250 baud the 2 KB buffer empties in
// 0.66 s, so waiting up to 0.5 s for room is enough for any reply.
constexpr unsigned SendTimeoutMicros = 500000;
} // namespace

bool CMT32Pi::SendGolemFrame(const u8* pData, size_t nSize)
{
	if (!m_bSerialMIDIEnabled || m_pSerial == nullptr)
		return false;

	const unsigned nStart = CTimer::GetClockTicks();
	while (m_pSerial->GetFreeWriteSpace() < nSize)
	{
		if (CTimer::GetClockTicks() - nStart > SendTimeoutMicros)
		{
			LOGERR("Golem reply of %u bytes dropped: serial port busy", static_cast<unsigned>(nSize));
			return false;
		}
		CTimer::SimpleusDelay(1000);
	}

	return m_pSerial->Write(pData, nSize) == static_cast<int>(nSize);
}

golem::ParamInfo CMT32Pi::Info()
{
	golem::ParamInfo Info;
	Info.Platform = 0; // Circle firmware

	// "golem-1.3.0", or "golem-1.3.0-4-gabcdef" between releases
	const char* pVersion = MT32_PI_VERSION;
	while (*pVersion && (*pVersion < '0' || *pVersion > '9'))
		++pVersion;
	unsigned a = 0, b = 0, c = 0;
	if (sscanf(pVersion, "%u.%u.%u", &a, &b, &c) == 3)
	{
		Info.Version[0] = static_cast<u8>(a & 0x7F);
		Info.Version[1] = static_cast<u8>(b & 0x7F);
		Info.Version[2] = static_cast<u8>(c & 0x7F);
	}

	Info.Engines   = (m_pMT32Synth ? 1 : 0) | (m_pSoundFontSynth ? 2 : 0) | (m_pOPL3Synth ? 4 : 0);
	Info.Functions = golem::param::FnAreas;
	if (m_pMT32Synth)
		Info.Functions |= golem::param::FnMT32RQ1 | golem::param::FnROMTimbres;
	if (m_pOPL3Synth)
		Info.Functions |= golem::param::FnOPL3Instr | golem::param::FnWOPLUp | golem::param::FnWOPLDown;
	if (m_pSoundFontSynth)
		Info.SoundFonts = static_cast<u16>(m_pSoundFontSynth->GetSoundFontManager().GetSoundFontCount() & 0x3FFF);
	if (m_pOPL3Synth)
	{
		Info.OPL3Chips      = static_cast<u8>(m_pOPL3Synth->GetChips());
		Info.FourOpChannels = static_cast<u8>(m_pOPL3Synth->GetFourOpChannels() & 0x7F);
		Info.OPL3Banks      = static_cast<u8>(m_pOPL3Synth->GetBankCount() & 0x7F);
	}
	Info.Name = "mt32-pi " MT32_PI_VERSION;
	return Info;
}

u8 CMT32Pi::AreaRead(u8 nArea, uint32_t nAddress, size_t nSize, std::vector<u8>& Out)
{
	Out.clear();
	if (nArea == golem::param::MT32 || nArea == golem::param::MT32ROM)
	{
		if (!m_pMT32Synth)
			return golem::param::Unavailable;
		return golem::mt32mem::Read(*this, nArea, nAddress, nSize, Out);
	}
	if (nArea == golem::param::OPL3Instr || nArea == golem::param::OPL3Banks)
		return ReadOPL3(nArea, nAddress, nSize, Out);
	if (nArea != golem::param::Settings)
		return golem::param::BadAddress;

	for (size_t i = 0; i < nSize; ++i)
	{
		const u32 a = nAddress + static_cast<u32>(i);
		if (((a >> 7) & 0x7F) != 0)
			return golem::param::BadAddress;
		u8 nValue = 0;
		if (const u8 nError = ReadSetting(Section(a), a & 0x7F, nValue))
			return nError;
		Out.push_back(nValue);
	}
	return golem::param::OK;
}

u8 CMT32Pi::ReadSetting(u32 nSection, u32 p, u8& v)
{
	if (nSection == 0)
	{
		if (p <= SetParameterLast)
		{
			u8 nMax;
			bool bAvailable;
			GetGolemParameterLimits(static_cast<u8>(p), nMax, bAvailable);
			v = bAvailable ? GetGolemParameter(static_cast<u8>(p)) : 0;
			return golem::param::OK;
		}

		switch (p)
		{
			case GeneralSynth:
				v = m_pCurrentSynth == m_pOPL3Synth && m_pOPL3Synth           ? static_cast<u8>(TSynth::OPL3) :
				    m_pCurrentSynth == m_pSoundFontSynth && m_pSoundFontSynth ? static_cast<u8>(TSynth::SoundFont) :
				    m_pCurrentSynth == m_pMT32Synth && m_pMT32Synth           ? static_cast<u8>(TSynth::MT32) :
				                                                                0x7F;
				return golem::param::OK;

			case GeneralROM:
				v = m_pMT32Synth ? static_cast<u8>(m_pMT32Synth->GetROMSet()) : 0x7F;
				return golem::param::OK;

			case GeneralSFLow:
			case GeneralSFHigh:
			{
				const size_t nSoundFont = m_pSoundFontSynth ? m_pSoundFontSynth->GetSoundFontIndex() : 0x3FFF;
				v = static_cast<u8>((p == GeneralSFLow ? nSoundFont : nSoundFont >> 7) & 0x7F);
				return golem::param::OK;
			}

			case GeneralUSBOut:
				v = static_cast<u8>(m_USBOutMode);
				return golem::param::OK;

			case GeneralUSBCount:
			{
				const size_t nDevices = GetUSBMIDIDeviceCount();
				v = static_cast<u8>(nDevices > 127 ? 127 : nDevices);
				return golem::param::OK;
			}

			case GeneralEngines:
				v = (m_pMT32Synth ? 1 : 0) | (m_pSoundFontSynth ? 2 : 0) | (m_pOPL3Synth ? 4 : 0);
				return golem::param::OK;

			default:
				return golem::param::BadAddress;
		}
	}

	if (nSection == 1)
		return ReadMT32Setting(p, v);

	if (nSection == 2)
	{
		if (!m_pSoundFontSynth)
			return golem::param::Unavailable;
		return golem::fxparams::Read(m_pSoundFontSynth->GetFX(), p, v);
	}

	if (nSection == 3)
	{
		if (!m_pOPL3Synth)
			return golem::param::Unavailable;
		const unsigned nGain = static_cast<unsigned>(m_pOPL3Synth->GetGain() * 100.0f + 0.5f);
		switch (p)
		{
			case OPL3Emulator: v = m_pOPL3Synth->IsDOSBox() ? 1 : 0; return golem::param::OK;
			case OPL3Chips:    v = static_cast<u8>(m_pOPL3Synth->GetChips()); return golem::param::OK;
			case OPL3GainLow:  v = nGain & 0x7F; return golem::param::OK;
			case OPL3GainHigh: v = (nGain >> 7) & 0x7F; return golem::param::OK;
			default:           return golem::param::BadAddress;
		}
	}

	return golem::param::BadAddress;
}

u8 CMT32Pi::AreaWrite(u8 nArea, uint32_t nAddress, const u8* pData, size_t nSize, size_t& nBad)
{
	if (nArea == golem::param::MT32)
	{
		nBad = 0;
		if (!m_pMT32Synth)
			return golem::param::Unavailable;
		return golem::mt32mem::Write(*this, nAddress, pData, nSize, nBad);
	}
	if (nArea == golem::param::OPL3Instr || nArea == golem::param::OPL3Banks)
		return WriteOPL3(nArea, nAddress, pData, nSize, nBad);
	if (nArea == golem::param::Settings)
		return WriteSettings(nAddress, pData, nSize, nBad);
	nBad = 0;
	return golem::param::BadAddress;
}

// Everything checked first, then applied, so a refused byte changes nothing
u8 CMT32Pi::WriteSettings(u32 nAddress, const u8* p, size_t n, size_t& nBad)
{
	const u32 nSection = Section(nAddress);
	if (nSection == 1)
		return WriteMT32Settings(nAddress, p, n, nBad);
	if (nSection == 2)
	{
		nBad = 0;
		if (!m_pSoundFontSynth)
			return golem::param::Unavailable;
		// One section, parameter byte only (ss 00 pp)
		if (Section(nAddress + static_cast<u32>(n) - 1) != 2 || ((nAddress >> 7) & 0x7F) != 0 ||
		    (nAddress & 0x7F) + n - 1 > golem::fxparams::Last)
		{
			nBad = (nAddress & 0x7F) > golem::fxparams::Last ? 0 : golem::fxparams::Last + 1 - (nAddress & 0x7F);
			return golem::param::BadAddress;
		}
		golem::FXSettings FX = m_pSoundFontSynth->GetFX();
		const u8 nError = golem::fxparams::Write(FX, nAddress & 0x7F, p, n, nBad);
		if (nError == golem::param::OK)
			m_pSoundFontSynth->SetFX(FX);
		return nError;
	}
	for (size_t i = 0; i < n; ++i)
	{
		const u32 a  = nAddress + static_cast<u32>(i);
		const u32 pp = a & 0x7F;
		nBad = i;
		if (Section(a) != nSection || ((a >> 7) & 0x7F) != 0)
			return golem::param::BadAddress;

		if (nSection == 0)
		{
			// 20-26 are read-only; the rest do not exist yet
			u8 nMax;
			bool bAvailable;
			if (pp > SetParameterLast || !GetGolemParameterLimits(static_cast<u8>(pp), nMax, bAvailable))
				return golem::param::BadAddress;
			if (!bAvailable)
				return golem::param::Unavailable;
			if (p[i] > nMax)
				return golem::param::BadValue;
		}
		else if (nSection == 3)
		{
			if (!m_pOPL3Synth)
				return golem::param::Unavailable;
			if (pp != OPL3Emulator && pp != OPL3Chips && pp != OPL3GainLow && pp != OPL3GainHigh)
				return golem::param::BadAddress;
			if ((pp == OPL3Emulator && p[i] > 1) || (pp == OPL3Chips && (p[i] < 1 || p[i] > 4)))
				return golem::param::BadValue;
		}
		else
			return golem::param::BadAddress;
	}
	nBad = 0;

	if (nSection == 0)
	{
		for (size_t i = 0; i < n; ++i)
			SetGolemParameter(static_cast<u8>((nAddress + i) & 0x7F), p[i]);
		return golem::param::OK;
	}

	// Section 03: the gain is two bytes; a write may carry one of them
	unsigned nGain = static_cast<unsigned>(m_pOPL3Synth->GetGain() * 100.0f + 0.5f);
	bool bGain = false;
	for (size_t i = 0; i < n; ++i)
	{
		const u32 pp = (nAddress + static_cast<u32>(i)) & 0x7F;
		if (pp == OPL3GainLow)
		{
			nGain = (nGain & ~0x7Fu) | p[i];
			bGain = true;
		}
		else if (pp == OPL3GainHigh)
		{
			nGain = (nGain & 0x7Fu) | (static_cast<unsigned>(p[i]) << 7);
			bGain = true;
		}
	}
	if (bGain && nGain > OPL3GainMax)
		return golem::param::BadValue;

	for (size_t i = 0; i < n; ++i)
	{
		const u32 pp = (nAddress + static_cast<u32>(i)) & 0x7F;
		if (pp == OPL3Emulator)
			m_pOPL3Synth->SetEmulator(p[i] != 0);
		else if (pp == OPL3Chips)
			m_pOPL3Synth->SetChips(p[i]);
	}
	if (bGain)
		m_pOPL3Synth->SetGain(nGain / 100.0f);
	return golem::param::OK;
}

u8 CMT32Pi::ReadOPL3(u8 nArea, u32 nAddress, size_t nSize, std::vector<u8>& Out)
{
	if (!m_pOPL3Synth)
		return golem::param::Unavailable;
	const golem::OPLBank* pBank = m_pOPL3Synth->GetWorkingBank();
	if (!pBank)
		return golem::param::Failed;

	if (nArea == golem::param::OPL3Banks)
	{
		for (size_t i = 0; i < nSize; ++i)
		{
			u8 nValue = 0;
			if (const u8 nError = pBank->ReadDirectory(nAddress + static_cast<u32>(i), nValue))
				return nError;
			Out.push_back(nValue);
		}
		return golem::param::OK;
	}

	// Area 01: whole records, then the slice asked for
	const u32 nFirst = nAddress >> 7, nLast = (nAddress + static_cast<u32>(nSize) - 1) >> 7;
	u8 Record[golem::OPLBank::RecordSize];
	for (u32 r = nFirst; r <= nLast; ++r)
	{
		if (!pBank->HasInstrument(static_cast<int>(r >> 7), static_cast<int>(r & 0x7F)))
			return golem::param::BadAddress;
		pBank->ReadRecord(static_cast<int>(r >> 7), static_cast<int>(r & 0x7F), Record);
		const size_t nFrom = r == nFirst ? (nAddress & 0x7F) : 0;
		const size_t nTo   = r == nLast ? ((nAddress + nSize - 1) & 0x7F) + 1 : golem::OPLBank::RecordSize;
		Out.insert(Out.end(), Record + nFrom, Record + nTo);
	}
	return golem::param::OK;
}

u8 CMT32Pi::WriteOPL3(u8 nArea, u32 nAddress, const u8* p, size_t n, size_t& nBad)
{
	if (!m_pOPL3Synth)
		return golem::param::Unavailable;
	const golem::OPLBank* pBank = m_pOPL3Synth->GetWorkingBank();
	if (!pBank)
		return golem::param::Failed;

	if (nArea == golem::param::OPL3Banks)
		return m_pOPL3Synth->WriteDirectory(nAddress, p, n, nBad);

	// Area 01: the records touched, with the new bytes laid over them
	const u32 nFirst = nAddress >> 7, nLast = (nAddress + static_cast<u32>(n) - 1) >> 7;
	const size_t nRecords = nLast - nFirst + 1;
	std::vector<u8> Records(nRecords * golem::OPLBank::RecordSize);
	for (u32 r = nFirst; r <= nLast; ++r)
	{
		if (!pBank->HasInstrument(static_cast<int>(r >> 7), static_cast<int>(r & 0x7F)))
		{
			nBad = r == nFirst ? 0 : (r - nFirst) * golem::OPLBank::RecordSize - (nAddress & 0x7F);
			return golem::param::BadAddress;
		}
		pBank->ReadRecord(static_cast<int>(r >> 7), static_cast<int>(r & 0x7F),
		                  Records.data() + (r - nFirst) * golem::OPLBank::RecordSize);
	}
	const size_t nStart = nAddress & 0x7F;
	memcpy(Records.data() + nStart, p, n);

	size_t nBadInRecords = 0;
	const u8 nError = m_pOPL3Synth->WriteRecords(static_cast<int>(nFirst), static_cast<int>(nRecords), Records.data(),
	                                             nBadInRecords);
	if (nError != golem::param::OK)
		nBad = nBadInRecords >= nStart ? nBadInRecords - nStart : 0;
	return nError;
}

u8 CMT32Pi::BlobCommit(u8 nType, u8 nTarget, std::vector<u8>&& File)
{
	if (nType == golem::param::BlobFX && nTarget == 0)
	{
		if (!m_pSoundFontSynth)
			return golem::param::Unavailable;
		golem::FXSettings FX = m_pSoundFontSynth->GetFX();
		size_t nLine = 0;
		if (!golem::fxparams::FromConfig(reinterpret_cast<const char*>(File.data()), File.size(), FX, nLine))
		{
			LOGWARN("Effects profile refused at line %u", static_cast<unsigned>(nLine));
			return golem::param::BadValue;
		}
		m_pSoundFontSynth->SetFX(FX);
		return golem::param::OK;
	}

	if (nType == golem::param::BlobMT32 && nTarget == 0)
	{
		if (!m_pMT32Synth)
			return golem::param::Unavailable;
		size_t nBad = 0;
		const u8 nError = golem::mt32mem::LoadDump(*this, File.data(), File.size(), nBad);
		if (nError != golem::param::OK)
			LOGWARN("MT-32 dump refused at byte %u", static_cast<unsigned>(nBad));
		return nError;
	}

	if (nType != golem::param::BlobWOPL || nTarget != 0 || !m_pOPL3Synth)
		return golem::param::Unavailable;

	golem::OPLBank Bank;
	if (!Bank.Load(File.data(), File.size()))
	{
		LOGWARN("Uploaded OPL3 bank is not a valid WOPL file");
		return golem::param::BadValue;
	}
	if (!m_pOPL3Synth->UploadBank(std::move(Bank)))
		return golem::param::Failed;
	LCDLog(TLCDLogType::Notice, "OPL3 bank uploaded");
	return golem::param::OK;
}

u8 CMT32Pi::BlobGet(u8 nType, u8 nOrigin, std::vector<u8>& File)
{
	if (nType == golem::param::BlobFX && nOrigin == 0)
	{
		if (!m_pSoundFontSynth)
			return golem::param::Unavailable;
		const std::string Text = golem::fxparams::ToConfig(m_pSoundFontSynth->GetFX());
		File.assign(Text.begin(), Text.end());
		return golem::param::OK;
	}

	if (nType == golem::param::BlobMT32 && nOrigin == 0)
	{
		if (!m_pMT32Synth)
			return golem::param::Unavailable;
		File = golem::mt32mem::Dump(*this);
		return golem::param::OK;
	}

	if (nType != golem::param::BlobWOPL || nOrigin != 0 || !m_pOPL3Synth)
		return golem::param::Unavailable;
	const golem::OPLBank* pBank = m_pOPL3Synth->GetWorkingBank();
	if (!pBank)
		return golem::param::Failed;
	File = pBank->Save();
	return File.empty() ? golem::param::Failed : golem::param::OK;
}

// Area 10, section 01: the MT-32 engine (sound-params.md 8.2)
u8 CMT32Pi::ReadMT32Setting(u32 p, u8& v)
{
	if (!m_pMT32Synth)
		return golem::param::Unavailable;
	const unsigned nGain = Hundredths(m_pMT32Synth->GetGain()), nReverbGain = Hundredths(m_pMT32Synth->GetReverbGain());
	switch (p)
	{
		case MT32GainLow:        v = nGain & 0x7F; return golem::param::OK;
		case MT32GainHigh:       v = (nGain >> 7) & 0x7F; return golem::param::OK;
		case MT32ReverbGainLow:  v = nReverbGain & 0x7F; return golem::param::OK;
		case MT32ReverbGainHigh: v = (nReverbGain >> 7) & 0x7F; return golem::param::OK;
		case MT32Resampler:      v = static_cast<u8>(m_pMT32Synth->GetResamplerQuality()); return golem::param::OK;
		case MT32Reversed:       v = m_pMT32Synth->GetReversedStereo() ? 1 : 0; return golem::param::OK;
		case MT32Reverb:         v = m_pMT32Synth->GetReverbMode(); return golem::param::OK;
		default:                 return golem::param::BadAddress;
	}
}

// Checked whole first, then applied, so a refused byte changes nothing
u8 CMT32Pi::WriteMT32Settings(u32 nAddress, const u8* p, size_t n, size_t& nBad)
{
	if (!m_pMT32Synth)
	{
		nBad = 0;
		return golem::param::Unavailable;
	}

	unsigned nGain = Hundredths(m_pMT32Synth->GetGain()), nReverbGain = Hundredths(m_pMT32Synth->GetReverbGain());
	bool bGains = false;
	for (size_t i = 0; i < n; ++i)
	{
		const u32 a  = nAddress + static_cast<u32>(i);
		const u32 pp = a & 0x7F;
		nBad = i;
		if (Section(a) != 1 || ((a >> 7) & 0x7F) != 0 || pp > MT32Reverb)
			return golem::param::BadAddress;
		switch (pp)
		{
			case MT32GainLow:        nGain = (nGain & ~0x7Fu) | p[i]; bGains = true; break;
			case MT32GainHigh:       nGain = (nGain & 0x7Fu) | (p[i] << 7); bGains = true; break;
			case MT32ReverbGainLow:  nReverbGain = (nReverbGain & ~0x7Fu) | p[i]; bGains = true; break;
			case MT32ReverbGainHigh: nReverbGain = (nReverbGain & 0x7Fu) | (p[i] << 7); bGains = true; break;
			case MT32Resampler:
				// None renders at Munt's own 32 kHz: only right with that output rate
				if (p[i] > 4 || (p[i] == 0 && m_pConfig->AudioSampleRate != 32000))
					return golem::param::BadValue;
				break;
			case MT32Reversed: if (p[i] > 1) return golem::param::BadValue; break;
			case MT32Reverb:   if (p[i] > 2) return golem::param::BadValue; break;
		}
	}
	if (bGains && (nGain > MT32GainMax || nReverbGain > MT32GainMax))
	{
		nBad = 0;
		return golem::param::BadValue;
	}
	nBad = 0;

	for (size_t i = 0; i < n; ++i)
	{
		switch ((nAddress + static_cast<u32>(i)) & 0x7F)
		{
			case MT32Resampler:
				m_pMT32Synth->SetResamplerQuality(static_cast<CMT32Synth::TResamplerQuality>(p[i]));
				break;
			case MT32Reversed: m_pMT32Synth->SetReversedStereo(p[i] != 0); break;
			case MT32Reverb:   m_pMT32Synth->SetReverbMode(p[i]); break;
			default:           break;
		}
	}
	if (bGains)
		m_pMT32Synth->SetGains(nGain / 100.0f, nReverbGain / 100.0f);
	return golem::param::OK;
}

void CMT32Pi::ReadMunt(uint32_t nAddress, size_t nSize, uint8_t* pOut)
{
	m_pMT32Synth->ReadMemory(nAddress, nSize, pOut);
}

void CMT32Pi::SendDT1(const uint8_t* pMessage, size_t nSize)
{
	// As a song's DT1: the kept channel assignment and volume still apply
	m_pMT32Synth->HandleMIDISysExMessage(pMessage, nSize);
}

// A native RQ1 from the Next while the MT-32 plays: the DT1 answers wait in a
// queue for SendRQ1Replies. Other units, channel-relative requests and
// unreadable zones get no answer, as on an MT-32.
bool CMT32Pi::AnswerMT32Request(const u8* pData, size_t nSize)
{
	uint32_t nAddress, nLength;
	if (!m_pMT32Synth || m_pCurrentSynth != m_pMT32Synth || !golem::mt32mem::IsRQ1(pData, nSize, nAddress, nLength))
		return false;

	auto Replies = golem::mt32mem::AnswerRQ1(*this, nAddress, nLength);
	for (auto& Reply : Replies)
	{
		if (m_RQ1Replies.size() >= RQ1QueueMax)
		{
			LOGWARN("RQ1 answers dropped: too many requests at once");
			break;
		}
		m_RQ1Replies.push_back(std::move(Reply));
	}
	return true;
}

// One whole DT1 at a time, when the serial port has room for all of it, and
// 20 ms after the previous one has left
void CMT32Pi::SendRQ1Replies()
{
	if (m_RQ1Replies.empty())
		return;
	if (!m_bSerialMIDIEnabled || m_pSerial == nullptr)
	{
		m_RQ1Replies.clear();
		return;
	}

	const unsigned nNow = CTimer::GetClockTicks();
	if (static_cast<int>(nNow - m_nRQ1NextTime) < 0)
		return;
	const std::vector<u8>& Reply = m_RQ1Replies.front();
	if (m_pSerial->GetFreeWriteSpace() < Reply.size())
		return;

	m_pSerial->Write(Reply.data(), Reply.size());
	m_nRQ1NextTime = nNow + static_cast<unsigned>(Reply.size()) * MicrosPerByte + RQ1GapMicros;
	m_RQ1Replies.pop_front();
}
