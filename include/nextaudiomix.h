//
// nextaudiomix.h
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
//
// Experimental: the ZX Spectrum Next's own audio, received on PCM_DIN (GPIO 20).
// With NextReg $A2 bit 4 set (the Golem driver writes $D2) the Next sends the
// output of its mixer back over the same I2S link: beeper, AY, DAC/Specdrum
// and the Pi itself, 13 bits per channel, standard I2S (one-bit delay, MSB
// first, then zeros). The Pi reads 24-bit slots, so a sample arrives as the
// 13-bit value shifted left by 11 and sign-extended to 32 bits.
//
// The Next's mixer is unsigned and its silence is not zero (the DAC and the Pi
// inputs rest at mid-scale), so the samples go through a DC blocker. A ring
// buffer joins the I2S receive interrupt to the audio task, which runs at
// the output device's rate: an empty ring gives silence, a full one drops the
// oldest samples.
//
// Kept free of Circle so the host tests use the same code
// (tests/test_nextaudiomix.cpp).
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

#ifndef _nextaudiomix_h
#define _nextaudiomix_h

#include <stddef.h>
#include <stdint.h>

namespace NextAudio
{
	// Full scale of a 13-bit sample in a 24-bit slot, sign-extended
	constexpr float FullScale = 4096.0f * 2048.0f;

	// One received word (I2S slot as Circle hands it over) as a float, -1..1
	inline float WordToSample(uint32_t nWord)
	{
		return static_cast<float>(static_cast<int32_t>(nWord)) / FullScale;
	}

	// Single-pole DC blocker, about 7.6 Hz at 48 kHz
	struct TDCBlocker
	{
		float nLastIn  = 0.0f;
		float nLastOut = 0.0f;
		bool bPrimed   = false;

		float Process(float nIn)
		{
			// Start from the first sample, so the offset does not thump
			if (!bPrimed)
			{
				nLastIn = nIn;
				bPrimed = true;
			}
			const float nOut = nIn - nLastIn + 0.999f * nLastOut;
			nLastIn  = nIn;
			nLastOut = nOut;
			return nOut;
		}
	};

	// Interleaved stereo frames from one producer (the receive interrupt) to
	// one consumer (the audio task), on different cores
	template <size_t NFrames>
	class CRing
	{
	public:
		static_assert((NFrames & (NFrames - 1)) == 0, "power of two");

		// Producer: frames of two words each
		void Put(const uint32_t* pWords, size_t nFrames)
		{
			// With its I2S off the Next leaves GPIO 20 floating and the Pi
			// reads random bits: a loud noise. Its real samples are 13 bits,
			// so the 11 bits below them always arrive as zeros. A chunk with
			// anything in the lowest 8 (a margin of 3 bits, should the slot
			// alignment differ from the model) is not the Next: silence
			// instead, and the DC blockers start again from the first good
			// sample.
			bool bValid = true;
			for (size_t i = 0; i < nFrames * 2 && bValid; ++i)
				bValid = (pWords[i] & LowBitsMask) == 0;
			if (!bValid)
			{
				++m_nInvalidChunks;
				m_DC[0] = TDCBlocker();
				m_DC[1] = TDCBlocker();
			}

			size_t nWrite = __atomic_load_n(&m_nWrite, __ATOMIC_RELAXED);
			const size_t nRead = __atomic_load_n(&m_nRead, __ATOMIC_ACQUIRE);
			for (size_t i = 0; i < nFrames; ++i)
			{
				if (nWrite - nRead >= NFrames)
				{
					// Full: the consumer is behind; count it and drop the rest
					m_nOverruns += nFrames - i;
					break;
				}
				const size_t n = nWrite & (NFrames - 1);
				m_Samples[n][0] = bValid ? m_DC[0].Process(WordToSample(pWords[i * 2])) : 0.0f;
				m_Samples[n][1] = bValid ? m_DC[1].Process(WordToSample(pWords[i * 2 + 1])) : 0.0f;
				++nWrite;
			}
			__atomic_store_n(&m_nWrite, nWrite, __ATOMIC_RELEASE);
		}

		// Consumer: adds nFrames of Next audio, times nGain, to interleaved
		// stereo floats, and clips to -1..1. Waits until half the ring has
		// filled before it starts (and after running dry), so small rate
		// differences between the two clocks do not empty it at once.
		void MixInto(float* pOut, size_t nFrames, float nGain)
		{
			const size_t nWrite = __atomic_load_n(&m_nWrite, __ATOMIC_ACQUIRE);
			size_t nRead = __atomic_load_n(&m_nRead, __ATOMIC_RELAXED);

			if (m_bWaiting && nWrite - nRead >= NFrames / 2)
				m_bWaiting = false;

			for (size_t i = 0; i < nFrames; ++i)
			{
				float L = 0.0f, R = 0.0f;
				if (!m_bWaiting)
				{
					if (nRead != nWrite)
					{
						const size_t n = nRead & (NFrames - 1);
						L = m_Samples[n][0] * nGain;
						R = m_Samples[n][1] * nGain;
						++nRead;
					}
					else
					{
						++m_nUnderruns;
						m_bWaiting = true;
					}
				}
				pOut[i * 2]     = Clip(pOut[i * 2] + L);
				pOut[i * 2 + 1] = Clip(pOut[i * 2 + 1] + R);
			}

			__atomic_store_n(&m_nRead, nRead, __ATOMIC_RELEASE);
		}

		size_t Level() const
		{
			return __atomic_load_n(&m_nWrite, __ATOMIC_ACQUIRE) - __atomic_load_n(&m_nRead, __ATOMIC_ACQUIRE);
		}

		size_t Received() const { return __atomic_load_n(&m_nWrite, __ATOMIC_ACQUIRE); }
		size_t Overruns() const { return m_nOverruns; }
		size_t InvalidChunks() const { return m_nInvalidChunks; }
		size_t Underruns() const { return m_nUnderruns; }

	private:
		static float Clip(float n) { return n > 1.0f ? 1.0f : n < -1.0f ? -1.0f : n; }

		float m_Samples[NFrames][2] = {};
		TDCBlocker m_DC[2];
		size_t m_nWrite = 0;
		size_t m_nRead  = 0;
		static constexpr uint32_t LowBitsMask = (1u << 8) - 1;

		volatile size_t m_nOverruns  = 0;
		volatile size_t m_nInvalidChunks = 0;
		size_t m_nUnderruns = 0;
		bool m_bWaiting = true;
	};
}

#endif
