//
// nextaudioinput.h
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
//
// Experimental ([audio] next_input): receives the ZX Spectrum Next's audio on
// I2S while the synth plays through another output (PWM or HDMI), so that one
// line carries the Next and the Golem together. The I2S link runs in both
// directions with the Pi as clock master; it sends silence to the Next, whose
// mix therefore holds no Golem audio to come back.
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

#ifndef _nextaudioinput_h
#define _nextaudioinput_h

#include <circle/interrupt.h>
#include <circle/sound/i2ssoundbasedevice.h>
#include <circle/types.h>

#include "nextaudiomix.h"

class CNextAudioInput : public CI2SSoundBaseDevice
{
public:
	// About 21 ms at 48 kHz; playback starts at half (about 11 ms of delay)
	static constexpr size_t RingFrames = 1024;

	CNextAudioInput(CInterruptSystem* pInterrupt, unsigned nSampleRate, unsigned nChunkSize);

	// Audio task: adds the Next's audio to the synth's output
	void MixInto(float* pOut, size_t nFrames, float nGain) { m_Ring.MixInto(pOut, nFrames, nGain); }

	// For the log: frames received, ring level, overruns, underruns
	void LogStatus() const;

protected:
	using CI2SSoundBaseDevice::GetChunk;
	using CI2SSoundBaseDevice::PutChunk;

	// Silence to the Next
	virtual unsigned GetChunk(u32* pBuffer, unsigned nChunkSize) override;
	// The Next's mix into the ring (interrupt context)
	virtual void PutChunk(const u32* pBuffer, unsigned nChunkSize) override;

private:
	NextAudio::CRing<RingFrames> m_Ring;
};

#endif
