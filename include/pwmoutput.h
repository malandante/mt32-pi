//
// pwmoutput.h
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
//
// The Pi's own headphone output (PWM) made as good as it gets, for a Golem
// heard without the Next's HDMI (VGA, a monitor without speakers). The PWM
// has few levels: about 2600 at 48 kHz on a Pi 4 (125 MHz clock), some 11
// bits, and Circle reaches them by truncating, so quiet sounds (reverb
// tails, fades) turn grainy. Three things help:
//
// - Oversampling x2: the output runs at twice the rate (a half-band
//   interpolator), so the quantization noise spreads over twice the band
//   and the shaping below has room above what is heard.
// - Dither: the level is chosen with a little random noise (TPDF), which
//   turns the grain on quiet sounds into a steady, much quieter hiss; with
//   "shaped" and the oversampling, that hiss is pushed up, above the audible
//   band (third-order error feedback, zeros at 0 and 12 kHz): about 10 dB
//   less, A-weighted, than plain dither at 48 kHz. At 48 kHz there is no
//   room for it, and "shaped" is plain dither.
// - Silence stays silence: a block that is all zero keeps the output still
//   at mid level, without dither, so pauses are quiet.
//
// The levels are computed here and handed to Circle as 24-bit samples that
// its conversion maps exactly to them. Kept free of Circle so the host tests
// use the same code (tests/test_pwmoutput.cpp).
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

#ifndef _pwmoutput_h
#define _pwmoutput_h

#include <math.h>
#include <stddef.h>
#include <stdint.h>

namespace PWMOutput
{
	// M_PI is not in every C library (newlib in strict mode)
	constexpr float Pi = 3.14159265358979f;

	enum class TDither
	{
		Off,
		On,	// TPDF
		Shaped,	// TPDF and second-order noise shaping
	};

	// Circle's conversion to the PWM (CSoundBaseDevice, SoundFormatUnsigned32):
	// level = ((sample24 << 8) + 2^31) * nRangeMax >> 32, levels 0 .. nRangeMax - 1
	inline uint32_t CircleLevel(int32_t nSample24, uint32_t nRangeMax)
	{
		int64_t n = static_cast<int64_t>(nSample24) * 256 + (int64_t(1) << 31);
		return static_cast<uint32_t>((n * nRangeMax) >> 32);
	}

	// The 24-bit sample that Circle turns into nLevel (the middle of its span)
	inline int32_t SampleForLevel(uint32_t nLevel, uint32_t nRangeMax)
	{
		const uint64_t nCentre = ((uint64_t(2) * nLevel + 1) << 31) / nRangeMax;
		const int64_t n32 = static_cast<int64_t>(nCentre) - (int64_t(1) << 31);
		return static_cast<int32_t>(n32 >> 8);
	}

	// x2 half-band interpolator, stereo interleaved: 48 taps, Kaiser window
	class CUpsampler2x
	{
	public:
		static constexpr int Half = 24;		// taps on each side of the gap

		CUpsampler2x()
		{
			// Odd outputs fall halfway between inputs: sinc at +-0.5, +-1.5...
			float nSum = 0.0f;
			for (int j = 0; j < 2 * Half; ++j)
			{
				const float t = j - Half + 0.5f;
				const float x = Pi * t;
				const float r = t / (Half + 0.5f);
				m_Taps[j] = sinf(x) / x * Kaiser(8.0f, r);
				nSum += m_Taps[j];
			}
			for (float& h : m_Taps)
				h /= nSum;
		}

		// nFrames in, 2 x nFrames out
		void Process(const float* pIn, size_t nFrames, float* pOut)
		{
			for (size_t i = 0; i < nFrames; ++i)
			{
				for (int c = 0; c < 2; ++c)
				{
					float* const pHistory = m_History[c];
					for (int j = 2 * Half - 1; j > 0; --j)
						pHistory[j] = pHistory[j - 1];
					pHistory[0] = pIn[i * 2 + c];

					// Even: the input Half samples ago; odd: halfway after it
					float nOdd = 0.0f;
					for (int j = 0; j < 2 * Half; ++j)
						nOdd += pHistory[j] * m_Taps[j];
					pOut[i * 4 + c] = pHistory[Half];
					pOut[i * 4 + 2 + c] = nOdd;
				}
			}
		}

	private:
		static float BesselI0(float x)
		{
			float nSum = 1.0f, nTerm = 1.0f;
			for (int k = 1; k < 25; ++k)
			{
				nTerm *= (x / (2.0f * k)) * (x / (2.0f * k));
				nSum += nTerm;
			}
			return nSum;
		}

		static float Kaiser(float nBeta, float r)
		{
			if (r <= -1.0f || r >= 1.0f)
				return 0.0f;
			return BesselI0(nBeta * sqrtf(1.0f - r * r)) / BesselI0(nBeta);
		}

		float m_Taps[2 * Half];
		float m_History[2][2 * Half] = {};
	};

	// Floats (-1..1, interleaved stereo) to 24-bit samples on the PWM levels
	class CQuantizer
	{
	public:
		void Configure(uint32_t nRangeMax, TDither Dither, unsigned nSampleRate)
		{
			m_nRangeMax = nRangeMax;
			m_Dither = Dither == TDither::Shaped && nSampleRate < 88200 ? TDither::On : Dither;
			// Noise transfer (1 - z^-1)(1 - c z^-1 + z^-2), zeros at 0 Hz and 12 kHz
			m_nC = 2.0f * cosf(2.0f * Pi * 12000.0f / nSampleRate);
		}

		void Process(const float* pIn, size_t nFrames, int32_t* pOut)
		{
			// Silence stays still: no dither, no leftover shaping error
			bool bSilent = true;
			for (size_t i = 0; i < nFrames * 2 && bSilent; ++i)
				bSilent = pIn[i] == 0.0f;
			if (bSilent)
			{
				const int32_t nMid = SampleForLevel(m_nRangeMax / 2, m_nRangeMax);
				for (size_t i = 0; i < nFrames * 2; ++i)
					pOut[i] = nMid;
				m_E1[0] = m_E1[1] = m_E2[0] = m_E2[1] = m_E3[0] = m_E3[1] = 0.0f;
				return;
			}

			const float nHalf = m_nRangeMax * 0.5f;
			const float nTop = static_cast<float>(m_nRangeMax - 1);
			for (size_t i = 0; i < nFrames * 2; ++i)
			{
				const int c = i & 1;
				float x = pIn[i];
				x = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;

				// The level as Circle would compute it, before truncation
				float nWanted = (x + 1.0f) * nHalf;
				if (m_Dither == TDither::Shaped)
					nWanted += (1.0f + m_nC) * (m_E2[c] - m_E1[c]) - m_E3[c];

				float nLevel = nWanted;
				if (m_Dither != TDither::Off)
					nLevel += Random() - Random();	// TPDF, +-1 level

				// Level L sounds as L + 0.5 on average (Circle truncates too)
				nLevel = floorf(nLevel);
				nLevel = nLevel < 0.0f ? 0.0f : nLevel > nTop ? nTop : nLevel;

				if (m_Dither == TDither::Shaped)
				{
					// The error the shaping feeds back, kept within reason so a
					// clipped stretch cannot make it run away
					float nError = nLevel + 0.5f - nWanted;
					nError = nError > 4.0f ? 4.0f : nError < -4.0f ? -4.0f : nError;
					m_E3[c] = m_E2[c];
					m_E2[c] = m_E1[c];
					m_E1[c] = nError;
				}

				pOut[i] = SampleForLevel(static_cast<uint32_t>(nLevel), m_nRangeMax);
			}
		}

	private:
		// 0..1, xorshift: cheap and enough for dither
		float Random()
		{
			m_nSeed ^= m_nSeed << 13;
			m_nSeed ^= m_nSeed >> 17;
			m_nSeed ^= m_nSeed << 5;
			return (m_nSeed >> 8) * (1.0f / 16777216.0f);
		}

		uint32_t m_nRangeMax = 2603;
		TDither m_Dither = TDither::Off;
		float m_nC = 0.0f;
		float m_E1[2] = {}, m_E2[2] = {}, m_E3[2] = {};
		uint32_t m_nSeed = 0x12345678;
	};
}

#endif
