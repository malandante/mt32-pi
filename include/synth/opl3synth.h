//
// opl3synth.h
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
//
// OPL3 FM synthesis (Sound Blaster 16 / AdLib sound) with libADLMIDI: General
// MIDI through one of its embedded instrument banks (0-78) or a WOPL bank file
// from the SD card's opl3 folder (79 onwards, in alphabetical order).
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

#ifndef _opl3synth_h
#define _opl3synth_h

#include <circle/string.h>
#include <circle/types.h>

#include <adlmidi.h>

#include "synth/opl3midi.h"
#include "synth/synthbase.h"

class COPL3Synth : public CSynthBase
{
public:
	// Bank numbers travel as one 7-bit Golem byte
	static constexpr int MaxBanks = 128;

	COPL3Synth(unsigned int nSampleRate);
	virtual ~COPL3Synth() override;

	// CSynthBase
	virtual bool Initialize() override;
	virtual void HandleMIDIShortMessage(u32 nMessage) override;
	virtual void HandleMIDISysExMessage(const u8* pData, size_t nSize) override;
	virtual bool IsActive() override { return m_bActive; }
	virtual void AllSoundOff() override;
	virtual void SetMasterVolume(u8 nVolume) override;
	virtual size_t Render(s16* pOutBuffer, size_t nFrames) override;
	virtual size_t Render(float* pOutBuffer, size_t nFrames) override;
	virtual void ReportStatus() const override;
	virtual void UpdateLCD(CLCD& LCD, unsigned int nTicks) override;

	// Embedded banks first, then the files of the opl3 folder
	bool SetBank(int nBank);
	int GetBank() const { return m_nBank; }
	int GetBankCount() const { return m_nEmbeddedBanks + m_nBankFiles; }
	const char* GetBankName(int nBank) const;
	const char* GetEmulatorName() const;

private:
	struct TBankFile
	{
		CString Path;
		CString Name;
	};

	void ScanBankFiles();
	ADL_MIDIPlayer* CreatePlayer(int nBank) const;
	void Rebuild();

	ADL_MIDIPlayer* m_pPlayer;
	int m_nEmulator;
	int m_nChips;
	int m_nBank;
	int m_nEmbeddedBanks;
	int m_nBankFiles;
	TBankFile m_BankFiles[MaxBanks];
	u8 m_nVolume;
	float m_nGain;
	volatile bool m_bActive;
	unsigned int m_nSilentFrames;
	OPL3MIDI::TSoundOffSweep m_SoundOffSweep;
};

#endif
