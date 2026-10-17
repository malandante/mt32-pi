//
// nextaudioinput.cpp
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
#include <circle/util.h>

#include "nextaudioinput.h"

LOGMODULE("nextaudio");

CNextAudioInput::CNextAudioInput(CInterruptSystem* pInterrupt, unsigned nSampleRate, unsigned nChunkSize)
	// Clock master, no DAC to probe, both directions
	: CI2SSoundBaseDevice(pInterrupt, nSampleRate, nChunkSize, false, nullptr, 0, DeviceModeTXRX)
{
}

unsigned CNextAudioInput::GetChunk(u32* pBuffer, unsigned nChunkSize)
{
	memset(pBuffer, 0, nChunkSize * sizeof(u32));
	return nChunkSize;
}

void CNextAudioInput::PutChunk(const u32* pBuffer, unsigned nChunkSize)
{
	// u32 is unsigned int and uint32_t unsigned long on 32-bit ARM: same width
	static_assert(sizeof(u32) == sizeof(uint32_t), "32-bit words");
	m_Ring.Put(reinterpret_cast<const uint32_t*>(pBuffer), nChunkSize / 2);
}

void CNextAudioInput::LogStatus() const
{
	LOGNOTE("Next audio: %u frames received, ring %u/%u, %u overruns, %u underruns",
	        static_cast<unsigned>(m_Ring.Received()), static_cast<unsigned>(m_Ring.Level()),
	        static_cast<unsigned>(RingFrames), static_cast<unsigned>(m_Ring.Overruns()),
	        static_cast<unsigned>(m_Ring.Underruns()));
}
