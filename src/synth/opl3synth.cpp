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
#include <circle/timer.h>
#include <circle/util.h>
#include <fatfs/ff.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "diagaudio.h"
#include "lcd/ui.h"
#include "synth/opl3midi.h"
#include "synth/opl3synth.h"

LOGMODULE("opl3synth");

// Below this peak a rendered buffer counts as silence
constexpr float SilencePeak = 1.0f / 32768.0f;

// WOPL bank files: SD:opl3/*.wopl. A full 128 + 128 instrument bank with
// 4-operator voices is about 20 KB; anything far larger is not a bank.
const char BankFileDirectory[] = "SD:opl3";
constexpr FSIZE_t MaxBankFileSize = 512 * 1024;

COPL3Synth::COPL3Synth(unsigned int nSampleRate)
	: CSynthBase(nSampleRate),
	  m_pPlayer(nullptr),
	  m_nEmulator(ADLMIDI_EMU_NUKED),
	  m_nChips(1),
	  m_nBank(0),
	  m_nEmbeddedBanks(0),
	  m_nBankFiles(0),
	  m_nVolume(100),
	  m_nGain(1.0f),
	  m_bActive(false),
	  m_nSilentFrames(0),
	  m_nCaptureCount(0),
	  m_nSweepTime(0),
	  m_nPostSweepFrames(~0u),
	  m_nPostSweepPeak(0.0f),
	  m_nPostSweepCrossings(0),
	  m_nLastSample(0.0f)
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

	m_nEmulator = pConfig->OPL3Emulator == CConfig::TOPL3Emulator::DOSBox ? ADLMIDI_EMU_DOSBOX : ADLMIDI_EMU_NUKED;
	m_nChips    = pConfig->OPL3Chips < 1 ? 1 : pConfig->OPL3Chips > 4 ? 4 : pConfig->OPL3Chips;
	m_nGain     = pConfig->OPL3Gain;

	m_nEmbeddedBanks = adl_getBanksCount();
	if (m_nEmbeddedBanks > MaxBanks)
		m_nEmbeddedBanks = MaxBanks;
	ScanBankFiles();

	// The configured bank, or the default one if it is missing (a bank file
	// that has gone, for example)
	int nBank = pConfig->OPL3Bank;
	if (nBank < 0 || nBank >= GetBankCount())
		nBank = 72 < m_nEmbeddedBanks ? 72 : 0;
	m_pPlayer = CreatePlayer(nBank);
	if (!m_pPlayer && nBank >= m_nEmbeddedBanks)
	{
		nBank = 72 < m_nEmbeddedBanks ? 72 : 0;
		m_pPlayer = CreatePlayer(nBank);
	}
	if (!m_pPlayer)
	{
		LOGERR("Failed to create the OPL3 player");
		return false;
	}
	m_nBank = nBank;

	LOGNOTE("%s, %d chip(s), bank %d: %s", GetEmulatorName(), m_nChips, m_nBank, GetBankName(m_nBank));
	return true;
}

void COPL3Synth::ScanBankFiles()
{
	DIR Dir;
	FILINFO FileInfo;
	const int nMaxFiles = MaxBanks - m_nEmbeddedBanks;

	m_nBankFiles = 0;
	FRESULT Result = f_findfirst(&Dir, &FileInfo, BankFileDirectory, "*");
	while (Result == FR_OK && *FileInfo.fname && m_nBankFiles < nMaxFiles)
	{
		const size_t nLength = strlen(FileInfo.fname);
		if (!(FileInfo.fattrib & (AM_DIR | AM_HID | AM_SYS)) && FileInfo.fname[0] != '.' &&
		    nLength > 5 && strcasecmp(FileInfo.fname + nLength - 5, ".wopl") == 0)
		{
			TBankFile& Entry = m_BankFiles[m_nBankFiles++];
			Entry.Path.Format("%s/%s", BankFileDirectory, FileInfo.fname);
			FileInfo.fname[nLength - 5] = '\0';
			Entry.Name = FileInfo.fname;
		}
		Result = f_findnext(&Dir, &FileInfo);
	}
	f_closedir(&Dir);

	// Alphabetical order, as the SoundFonts
	for (int i = 1; i < m_nBankFiles; ++i)
		for (int j = i; j > 0 && strcasecmp(m_BankFiles[j - 1].Name, m_BankFiles[j].Name) > 0; --j)
		{
			TBankFile Swap = m_BankFiles[j];
			m_BankFiles[j] = m_BankFiles[j - 1];
			m_BankFiles[j - 1] = Swap;
		}

	for (int i = 0; i < m_nBankFiles; ++i)
		LOGNOTE("Bank %d: %s", m_nEmbeddedBanks + i, static_cast<const char*>(m_BankFiles[i].Path));
}

ADL_MIDIPlayer* COPL3Synth::CreatePlayer(int nBank) const
{
	if (nBank < m_nEmbeddedBanks)
		return OPL3MIDI::NewPlayer(m_nSampleRate, m_nEmulator, m_nChips, nBank);

	// A bank file, read whole into memory; libADLMIDI copies the instruments
	const char* pPath = m_BankFiles[nBank - m_nEmbeddedBanks].Path;
	FIL File;
	if (f_open(&File, pPath, FA_READ) != FR_OK)
	{
		LOGERR("Cannot open %s", pPath);
		return nullptr;
	}

	const FSIZE_t nSize = f_size(&File);
	u8* pData = nSize > 0 && nSize <= MaxBankFileSize ? new u8[nSize] : nullptr;
	UINT nRead = 0;
	const bool bRead = pData && f_read(&File, pData, nSize, &nRead) == FR_OK && nRead == nSize;
	f_close(&File);

	ADL_MIDIPlayer* pPlayer = bRead ? OPL3MIDI::NewPlayer(m_nSampleRate, m_nEmulator, m_nChips, 0, pData, nSize) : nullptr;
	delete[] pData;

	if (!pPlayer)
		LOGERR("%s is not a valid WOPL bank", pPath);
	return pPlayer;
}

void COPL3Synth::HandleMIDIShortMessage(u32 nMessage)
{
	Capture(nMessage);

	m_Lock.Acquire();
	OPL3MIDI::ShortMessage(m_pPlayer, nMessage);
	const bool bStopped = m_SoundOffSweep.Message(nMessage);
	m_Lock.Release();

	// Diagnostic build: no new player at the stop sweep, so a note left
	// sounding stays, and the render measures it 2-4 s later
	if (bStopped)
	{
		m_nSweepTime = CTimer::GetClockTicks();
		m_nPostSweepPeak = 0.0f;
		m_nPostSweepCrossings = 0;
		m_nPostSweepFrames = 0;
		LOGNOTE("Stop sweep at %u us", static_cast<unsigned>(m_nSweepTime));
	}

	// Note On from silence: active straight away for the power-save logic
	if ((nMessage & 0xF0) == 0x90 && ((nMessage >> 16) & 0x7F))
		m_bActive = true;

	CSynthBase::HandleMIDIShortMessage(nMessage);
}

void COPL3Synth::HandleMIDISysExMessage(const u8* pData, size_t nSize)
{
	// GM System On (.golem reset, and play before each song): a new player,
	// so nothing from the previous song is left on the chip
	const u8 GMSystemOn[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
	Capture(0xF0 | static_cast<u32>(nSize) << 8);
	if (nSize == sizeof(GMSystemOn) && memcmp(pData, GMSystemOn, nSize) == 0)
	{
		// .golem reset after a stop: the capture first, then a new player
		WriteCapture("GM System On");
		Rebuild();
		return;
	}

	// GS/XG resets, master volume and the like
	m_Lock.Acquire();
	adl_rt_systemExclusive(m_pPlayer, pData, nSize);
	m_Lock.Release();
}

void COPL3Synth::Capture(u32 nMessage)
{
	TCapture& Entry = m_Capture[m_nCaptureCount++ % CaptureSize];
	Entry.nTime = CTimer::GetClockTicks();
	Entry.nMessage = nMessage;
}

// SD:opl3cap.txt: a header, then one line per message, oldest first:
// "milliseconds status data1 data2" in hex, as tests/smf_events.py prints, or
// "milliseconds f0 length" for a SysEx message. Overwritten each time.
void COPL3Synth::WriteCapture(const char* pReason)
{
	FIL File;
	if (f_open(&File, "SD:opl3cap.txt", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		LOGERR("Cannot write SD:opl3cap.txt");
		return;
	}

	const bool bMeasured = m_nPostSweepFrames >= m_nSampleRate * 4;
	CString Line;
	Line.Format("# opl3 capture (%s): bank %d %s, %s, %d chip(s), gain %.2f, volume %d\n"
	            "# 2-4 s after the stop sweep: %s peak %.6f, %u zero crossings/2 s\n"
	            "# %u messages received, the last %u below; sweep at %u us\n",
	            pReason, m_nBank, GetBankName(m_nBank), GetEmulatorName(), m_nChips, m_nGain, m_nVolume,
	            bMeasured ? "" : "(incomplete)", m_nPostSweepPeak, m_nPostSweepCrossings,
	            m_nCaptureCount, m_nCaptureCount < CaptureSize ? m_nCaptureCount : CaptureSize,
	            static_cast<unsigned>(m_nSweepTime));
	UINT nWritten;
	f_write(&File, static_cast<const char*>(Line), Line.GetLength(), &nWritten);

	const unsigned nCount = m_nCaptureCount < CaptureSize ? m_nCaptureCount : CaptureSize;
	const unsigned nFirst = m_nCaptureCount - nCount;
	const u32 nBase = m_Capture[nFirst % CaptureSize].nTime;
	for (unsigned i = 0; i < nCount; ++i)
	{
		const TCapture& Entry = m_Capture[(nFirst + i) % CaptureSize];
		const unsigned nMillis = (Entry.nTime - nBase) / 1000;
		const u32 nMessage = Entry.nMessage;
		if ((nMessage & 0xFF) == 0xF0)
			Line.Format("%u f0 %u\n", nMillis, static_cast<unsigned>(nMessage >> 8));
		else
			Line.Format("%u %02x %02x %02x\n", nMillis, static_cast<unsigned>(nMessage & 0xFF),
			            static_cast<unsigned>((nMessage >> 8) & 0xFF), static_cast<unsigned>((nMessage >> 16) & 0xFF));
		f_write(&File, static_cast<const char*>(Line), Line.GetLength(), &nWritten);
	}
	// One line per second of audio: time (ms, same base), synth render peak
	// and zero crossings, output peak and zero crossings
	Line.Format("# audio: ms synth_peak synth_crossings output_peak output_crossings (last %u s)\n",
	            DiagAudio::nWindows < DiagAudio::WindowCount ? DiagAudio::nWindows : DiagAudio::WindowCount);
	f_write(&File, static_cast<const char*>(Line), Line.GetLength(), &nWritten);
	const unsigned nTotal = DiagAudio::nWindows;
	const unsigned nShown = nTotal < DiagAudio::WindowCount ? nTotal : DiagAudio::WindowCount;
	for (unsigned i = nTotal - nShown; i < nTotal; ++i)
	{
		const DiagAudio::TWindow& W = DiagAudio::Windows[i % DiagAudio::WindowCount];
		Line.Format("# a %d %.6f %u %.6f %u\n", static_cast<int>(W.nTime - nBase) / 1000, W.nSynthPeak,
		            static_cast<unsigned>(W.nSynthCrossings), W.nOutputPeak, static_cast<unsigned>(W.nOutputCrossings));
		f_write(&File, static_cast<const char*>(Line), Line.GetLength(), &nWritten);
	}
	f_close(&File);

	LOGNOTE("Capture written: %u messages, post-sweep peak %.6f", nCount, m_nPostSweepPeak);
	m_nPostSweepFrames = ~0u;
}

void COPL3Synth::Rebuild()
{
	ADL_MIDIPlayer* const pPlayer = CreatePlayer(m_nBank);
	if (!pPlayer)
		return;

	m_Lock.Acquire();
	ADL_MIDIPlayer* const pOldPlayer = m_pPlayer;
	m_pPlayer = pPlayer;
	m_Lock.Release();
	adl_close(pOldPlayer);
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

	// Diagnostic: peak and zero crossings (left) from 2 to 4 s after a sweep
	if (m_nPostSweepFrames != ~0u)
	{
		const unsigned nStart = m_nSampleRate * 2, nEnd = m_nSampleRate * 4;
		if (m_nPostSweepFrames >= nStart && m_nPostSweepFrames < nEnd)
		{
			float nMax = m_nPostSweepPeak;
			unsigned nCrossings = m_nPostSweepCrossings;
			for (size_t i = 0; i < nFrames; ++i)
			{
				const float nSample = pOutBuffer[i * 2];
				const float nMagnitude = nSample < 0.0f ? -nSample : nSample;
				if (nMagnitude > nMax)
					nMax = nMagnitude;
				if ((nSample > 0.0f) != (m_nLastSample > 0.0f))
					++nCrossings;
				m_nLastSample = nSample;
			}
			m_nPostSweepPeak = nMax;
			m_nPostSweepCrossings = nCrossings;
		}
		if (m_nPostSweepFrames < nEnd)
			m_nPostSweepFrames += nFrames;
	}

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

	// A new player, built outside the lock (a file takes a moment to read)
	ADL_MIDIPlayer* const pPlayer = CreatePlayer(nBank);
	if (!pPlayer)
		return false;

	m_Lock.Acquire();
	ADL_MIDIPlayer* const pOldPlayer = m_pPlayer;
	m_pPlayer = pPlayer;
	m_nBank = nBank;
	m_Lock.Release();
	adl_close(pOldPlayer);

	m_MIDIMonitor.AllNotesOff();
	m_MIDIMonitor.ResetControllers(false);
	LOGNOTE("Bank %d: %s", m_nBank, GetBankName(m_nBank));
	return true;
}

const char* COPL3Synth::GetBankName(int nBank) const
{
	if (nBank < 0 || nBank >= GetBankCount())
		return "OPL3";
	if (nBank < m_nEmbeddedBanks)
		return adl_getBankNames()[nBank];
	return m_BankFiles[nBank - m_nEmbeddedBanks].Name;
}

const char* COPL3Synth::GetEmulatorName() const
{
	return m_pPlayer ? adl_chipEmulatorName(m_pPlayer) : "none";
}
