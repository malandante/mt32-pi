//
// opl3midi.h
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
//
// MIDI to libADLMIDI's real-time calls, and the float render with gain and
// clipping. Kept free of Circle so the host tests use the same code as the
// kernel (tests/test_opl3midi.cpp).
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

#ifndef _opl3midi_h
#define _opl3midi_h

#include <stddef.h>
#include <stdint.h>

#include <adlmidi.h>

namespace OPL3MIDI
{
	// A player as the kernel sets it up: emulator, chips, soft panning, and
	// either embedded bank nBank or, with pBankData, a WOPL bank file read
	// into memory. Changing banks makes a new player, so the sound never
	// depends on the banks used before (adl_setBank on a running player keeps
	// some of the previous bank's settings). nullptr if anything fails.
	inline ADL_MIDIPlayer* NewPlayer(unsigned nSampleRate, int nEmulator, int nChips, int nBank,
	                                 const void* pBankData = nullptr, size_t nBankSize = 0)
	{
		ADL_MIDIPlayer* pPlayer = adl_init(nSampleRate);
		if (!pPlayer)
			return nullptr;

		adl_switchEmulator(pPlayer, nEmulator);
		adl_setNumChips(pPlayer, nChips);
		adl_setSoftPanEnabled(pPlayer, 1);

		const int nResult = pBankData ? adl_openBankData(pPlayer, pBankData, nBankSize)
		                              : adl_setBank(pPlayer, nBank);
		if (nResult < 0)
		{
			adl_close(pPlayer);
			return nullptr;
		}

		return pPlayer;
	}

	// A short message as mt32-pi passes it: status in the low byte, then the
	// data bytes.
	inline void ShortMessage(ADL_MIDIPlayer* pPlayer, uint32_t nMessage)
	{
		const uint8_t nStatus  = nMessage & 0xFF;
		const uint8_t nChannel = nMessage & 0x0F;
		const uint8_t nData1   = (nMessage >> 8) & 0x7F;
		const uint8_t nData2   = (nMessage >> 16) & 0x7F;

		// System reset
		if (nStatus == 0xFF)
		{
			adl_panic(pPlayer);
			adl_rt_resetState(pPlayer);
			return;
		}

		switch (nStatus & 0xF0)
		{
			case 0x80:
				adl_rt_noteOff(pPlayer, nChannel, nData1);
				break;

			case 0x90:
				if (nData2)
					adl_rt_noteOn(pPlayer, nChannel, nData1, nData2);
				else
					adl_rt_noteOff(pPlayer, nChannel, nData1);
				break;

			case 0xA0:
				adl_rt_noteAfterTouch(pPlayer, nChannel, nData1, nData2);
				break;

			case 0xB0:
				adl_rt_controllerChange(pPlayer, nChannel, nData1, nData2);
				break;

			case 0xC0:
				adl_rt_patchChange(pPlayer, nChannel, nData1);
				break;

			case 0xD0:
				adl_rt_channelAfterTouch(pPlayer, nChannel, nData1);
				break;

			case 0xE0:
				adl_rt_pitchBendML(pPlayer, nChannel, nData2, nData1);
				break;

			default:
				break;
		}
	}

	// Interleaved stereo floats, scaled by nGain and kept within -1..1 (the
	// audio task converts to integers without clipping). Returns the peak
	// before clipping, so the caller can tell silence from sound.
	inline float Render(ADL_MIDIPlayer* pPlayer, float* pOut, size_t nFrames, float nGain)
	{
		ADLMIDI_AudioFormat Format;
		Format.type          = ADLMIDI_SampleType_F32;
		Format.containerSize = sizeof(float);
		Format.sampleOffset  = 2 * sizeof(float);

		adl_generateFormat(pPlayer, static_cast<int>(nFrames * 2),
		                   reinterpret_cast<ADL_UInt8*>(pOut),
		                   reinterpret_cast<ADL_UInt8*>(pOut + 1), &Format);

		float nPeak = 0.0f;
		for (size_t i = 0; i < nFrames * 2; ++i)
		{
			float nSample = pOut[i] * nGain;
			const float nMagnitude = nSample < 0.0f ? -nSample : nSample;
			if (nMagnitude > nPeak)
				nPeak = nMagnitude;
			if (nSample > 1.0f)
				nSample = 1.0f;
			else if (nSample < -1.0f)
				nSample = -1.0f;
			pOut[i] = nSample;
		}

		return nPeak;
	}
}

#endif
