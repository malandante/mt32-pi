//
// opl3synth.cpp
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
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

#include "config.h"
#include "lcd/ui.h"
#include "synth/opl3midi.h"
#include "synth/opl3synth.h"

LOGMODULE("opl3synth");

// Below this peak a rendered buffer counts as silence
constexpr float SilencePeak = 1.0f / 32768.0f;

COPL3Synth::COPL3Synth(unsigned int nSampleRate)
	: CSynthBase(nSampleRate),
	  m_pPlayer(nullptr),
	  m_nBank(0),
	  m_nVolume(100),
	  m_nGain(1.0f),
	  m_bActive(false),
	  m_nSilentFrames(0)
{
}

COPL3Synth::~COPL3Synth()
{
	if (m_pPlayer)
		adl_close(m_pPlayer);
}

bool COPL3Synth::Initialize()
{
	const CConfig* const pConfig = CConfig::Get();

	m_pPlayer = adl_init(m_nSampleRate);
	if (!m_pPlayer)
	{
		LOGERR("Failed to create the OPL3 player");
		return false;
	}

	const int nEmulator = pConfig->OPL3Emulator == CConfig::TOPL3Emulator::DOSBox ? ADLMIDI_EMU_DOSBOX : ADLMIDI_EMU_NUKED;
	if (adl_switchEmulator(m_pPlayer, nEmulator) < 0)
		LOGWARN("Emulator unavailable: %s", adl_errorInfo(m_pPlayer));

	int nChips = pConfig->OPL3Chips;
	if (nChips < 1)
		nChips = 1;
	else if (nChips > 4)
		nChips = 4;
	adl_setNumChips(m_pPlayer, nChips);
	adl_setSoftPanEnabled(m_pPlayer, 1);

	m_nGain = pConfig->OPL3Gain;

	int nBank = pConfig->OPL3Bank;
	if (nBank < 0 || nBank >= GetBankCount())
		nBank = 0;
	if (adl_setBank(m_pPlayer, nBank) < 0)
	{
		LOGERR("Bank %d: %s", nBank, adl_errorInfo(m_pPlayer));
		adl_close(m_pPlayer);
		m_pPlayer = nullptr;
		return false;
	}
	m_nBank = nBank;

	LOGNOTE("%s, %d chip(s), bank %d: %s", GetEmulatorName(), nChips, m_nBank, GetBankName(m_nBank));
	return true;
}

void COPL3Synth::HandleMIDIShortMessage(u32 nMessage)
{
	m_Lock.Acquire();
	OPL3MIDI::ShortMessage(m_pPlayer, nMessage);
	m_Lock.Release();

	// Note On from silence: active straight away for the power-save logic
	if ((nMessage & 0xF0) == 0x90 && ((nMessage >> 16) & 0x7F))
		m_bActive = true;

	CSynthBase::HandleMIDIShortMessage(nMessage);
}

void COPL3Synth::HandleMIDISysExMessage(const u8* pData, size_t nSize)
{
	// GM/GS/XG resets, master volume and the like
	m_Lock.Acquire();
	adl_rt_systemExclusive(m_pPlayer, pData, nSize);
	m_Lock.Release();
}

void COPL3Synth::AllSoundOff()
{
	m_Lock.Acquire();
	adl_panic(m_pPlayer);
	m_Lock.Release();

	CSynthBase::AllSoundOff();
}

void COPL3Synth::SetMasterVolume(u8 nVolume)
{
	m_nVolume = nVolume;
}

size_t COPL3Synth::Render(float* pOutBuffer, size_t nFrames)
{
	m_Lock.Acquire();
	const float nPeak = OPL3MIDI::Render(m_pPlayer, pOutBuffer, nFrames, m_nGain * m_nVolume / 100.0f);
	m_Lock.Release();

	// Active until half a second of silence
	if (nPeak > SilencePeak)
	{
		m_nSilentFrames = 0;
		m_bActive = true;
	}
	else if (m_bActive)
	{
		m_nSilentFrames += nFrames;
		if (m_nSilentFrames > m_nSampleRate / 2)
			m_bActive = false;
	}

	return nFrames;
}

size_t COPL3Synth::Render(s16* pOutBuffer, size_t nFrames)
{
	// Not used by the audio task; through floats in small blocks
	float Buffer[256 * 2];
	size_t nDone = 0;
	while (nDone < nFrames)
	{
		size_t nBlock = nFrames - nDone;
		if (nBlock > 256)
			nBlock = 256;
		Render(Buffer, nBlock);
		for (size_t i = 0; i < nBlock * 2; ++i)
			pOutBuffer[nDone * 2 + i] = static_cast<s16>(Buffer[i] * 32767.0f);
		nDone += nBlock;
	}
	return nFrames;
}

void COPL3Synth::ReportStatus() const
{
	if (m_pUI)
		m_pUI->ShowSystemMessage(GetBankName(m_nBank));
}

void COPL3Synth::UpdateLCD(CLCD& LCD, unsigned int nTicks)
{
	const u8 nBarHeight = LCD.Height();
	float ChannelLevels[16], PeakLevels[16];
	m_MIDIMonitor.GetChannelLevels(nTicks, ChannelLevels, PeakLevels, 1 << 9);
	CUserInterface::DrawChannelLevels(LCD, nBarHeight, ChannelLevels, PeakLevels, 16, true);
}

bool COPL3Synth::SetBank(int nBank)
{
	if (nBank < 0 || nBank >= GetBankCount())
		return false;

	m_Lock.Acquire();
	const int nResult = adl_setBank(m_pPlayer, nBank);
	m_Lock.Release();

	if (nResult < 0)
	{
		LOGERR("Bank %d: %s", nBank, adl_errorInfo(m_pPlayer));
		return false;
	}

	m_nBank = nBank;
	m_MIDIMonitor.AllNotesOff();
	LOGNOTE("Bank %d: %s", m_nBank, GetBankName(m_nBank));
	return true;
}

int COPL3Synth::GetBankCount()
{
	return adl_getBanksCount();
}

const char* COPL3Synth::GetBankName(int nBank)
{
	if (nBank < 0 || nBank >= GetBankCount())
		return "OPL3";
	return adl_getBankNames()[nBank];
}

const char* COPL3Synth::GetEmulatorName() const
{
	return m_pPlayer ? adl_chipEmulatorName(m_pPlayer) : "none";
}
