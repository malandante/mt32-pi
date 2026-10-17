//
// loudness.h
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
//
// Golem loudness ([golem] loudness): raises the synth's level by a fixed gain
// and keeps the peaks under full scale with a look-ahead limiter, so the
// average level goes up without clipping. Meant for the Next's HDMI path,
// where the Pi enters the Next's mixer with at most half the weight of its DAC
// (PCM): the synth needs all the level it can get there.
//
// Stereo-linked. The signal is delayed by the look-ahead (64 frames, 1.3 ms
// at 48 kHz) so the gain is already down when a peak arrives; it comes back up
// over about 200 ms. A soft clip at the end catches what the limiter misses.
//
// Kept free of Circle so the host tests use the same code
// (tests/test_loudness.cpp).
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

#ifndef _loudness_h
#define _loudness_h

#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace Loudness
{
	class CLimiter
	{
	public:
		static constexpr size_t Lookahead = 64;
		static constexpr float Threshold = 0.89f;	// -1 dB
		static constexpr float KneeStart = 0.95f;	// soft clip from here to 1.0

		void Configure(unsigned nSampleRate, float nGain)
		{
			m_nGain = nGain < 1.0f ? 1.0f : nGain;
			// Attack: down to the target within the look-ahead
			m_nAttack = 1.0f - expf(-5.0f / Lookahead);
			// Release: time constant of 200 ms
			m_nRelease = 1.0f - expf(-1.0f / (0.2f * static_cast<float>(nSampleRate)));
		}

		// In place, interleaved stereo
		void Process(float* pSamples, size_t nFrames)
		{
			for (size_t i = 0; i < nFrames; ++i)
			{
				float& L = pSamples[i * 2];
				float& R = pSamples[i * 2 + 1];
				const float xL = L * m_nGain;
				const float xR = R * m_nGain;

				// Gain this frame needs, and the lowest needed over the look-ahead
				const float nPeak = fmaxf(fabsf(xL), fabsf(xR));
				const float nTarget = nPeak > Threshold ? Threshold / nPeak : 1.0f;
				PushTarget(nTarget);
				const float nNeeded = m_Queue[m_nHead].nGain;

				m_nEnvelope += (nNeeded - m_nEnvelope) * (nNeeded < m_nEnvelope ? m_nAttack : m_nRelease);

				// Out: the frame from Lookahead frames ago
				const size_t n = m_nTime % Lookahead;
				const float oL = m_Delay[n][0] * m_nEnvelope;
				const float oR = m_Delay[n][1] * m_nEnvelope;
				m_Delay[n][0] = xL;
				m_Delay[n][1] = xR;
				++m_nTime;

				L = SoftClip(oL);
				R = SoftClip(oR);
			}
		}

		float GetGain() const { return m_nGain; }
		float GetEnvelope() const { return m_nEnvelope; }

	private:
		// Sliding minimum over the look-ahead (monotonic queue)
		void PushTarget(float nTarget)
		{
			while (m_nTail != m_nHead && m_Queue[(m_nTail - 1) & QueueMask].nGain >= nTarget)
				m_nTail = (m_nTail - 1) & QueueMask;
			m_Queue[m_nTail] = {m_nTime, nTarget};
			m_nTail = (m_nTail + 1) & QueueMask;
			while (m_nTime - m_Queue[m_nHead].nTime >= Lookahead)
				m_nHead = (m_nHead + 1) & QueueMask;
		}

		static float SoftClip(float x)
		{
			const float a = fabsf(x);
			if (a <= KneeStart)
				return x;
			const float y = KneeStart + (1.0f - KneeStart) * tanhf((a - KneeStart) / (1.0f - KneeStart));
			return x < 0.0f ? -y : y;
		}

		static constexpr size_t QueueSize = 128;
		static constexpr size_t QueueMask = QueueSize - 1;
		static_assert(QueueSize > Lookahead, "queue holds the window");

		struct TEntry
		{
			uint32_t nTime;
			float nGain;
		};

		float m_nGain     = 1.0f;
		float m_nAttack   = 1.0f;
		float m_nRelease  = 1.0f;
		float m_nEnvelope = 1.0f;
		float m_Delay[Lookahead][2] = {};
		uint32_t m_nTime = 0;
		TEntry m_Queue[QueueSize] = {};
		size_t m_nHead  = 0;
		size_t m_nTail  = 0;
	};
}

#endif
