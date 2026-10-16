//
// dither.h
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
// Copyright (C) 2020-2023 Dale Whinham <daleyo@gmail.com>
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

#ifndef _dither_h
#define _dither_h

// Converts float samples to signed 24-bit integers already reduced to a
// lower bit depth, with TPDF dither. For a receiver that keeps only the top
// bits of each sample (the ZX Spectrum Next takes 10 bits from the Pi's I2S),
// truncation of a quiet signal (a decaying note, a reverb tail) is heard as
// crackling; dithered rounding turns that error into a steady, faint hiss.
// The dither stops after a stretch of digital silence, so an idle synth
// outputs exact zeros instead of hiss.
//
// Kept free of Circle dependencies so it can be unit-tested on the host.

#include <stddef.h>
#include <stdint.h>

class CDither
{
public:
	static constexpr int32_t Max24 = (1 << 23) - 1;
	static constexpr int32_t Min24 = -(1 << 23);

	// nBits: effective bits at the receiver (2-23), 0 = off (plain conversion)
	// nSilenceFrames: frames below a fraction of a step before dither stops
	CDither(unsigned nBits = 0, size_t nSilenceFrames = 4800)
		: m_nBits(0), m_nStep(1), m_fStep(1.0f), m_fSilence(0.0f),
		  m_nSilenceLimit(nSilenceFrames), m_nSilentFrames(nSilenceFrames),
		  m_nSeed(0x6D2B79F5u)
	{
		SetBits(nBits);
	}

	void SetBits(unsigned nBits)
	{
		m_nBits = (nBits >= 2 && nBits <= 23) ? nBits : 0;
		m_nStep = m_nBits ? (1 << (24 - m_nBits)) : 1;
		m_fStep = static_cast<float>(m_nStep);
		m_fSilence = m_fStep / 16.0f;
	}

	unsigned GetBits() const { return m_nBits; }
	int32_t GetStep() const { return m_nStep; }

	// Interleaved stereo: pIn has nFrames * 2 floats in [-1, 1]; pOut gets
	// nFrames * 2 samples. bSwap swaps left and right.
	void Convert(const float* pIn, int32_t* pOut, size_t nFrames, bool bSwap)
	{
		const size_t nSamples = nFrames * 2;

		if (!m_nBits)
		{
			for (size_t i = 0; i < nSamples; ++i)
				pOut[i ^ bSwap] = Clamp(pIn[i] * Max24);
			return;
		}

		// Silence detection over the whole chunk, before converting it
		bool bSilent = true;
		for (size_t i = 0; i < nSamples && bSilent; ++i)
		{
			const float fValue = pIn[i] * Max24;
			bSilent = fValue < m_fSilence && fValue > -m_fSilence;
		}

		if (bSilent)
		{
			if (m_nSilentFrames < m_nSilenceLimit)
				m_nSilentFrames += nFrames;
		}
		else
			m_nSilentFrames = 0;

		const bool bDither = m_nSilentFrames < m_nSilenceLimit;

		for (size_t i = 0; i < nSamples; ++i)
		{
			float fValue = pIn[i] * Max24 / m_fStep;

			// Triangular noise of +/- 1 step: the sum of two uniform values
			if (bDither)
				fValue += Random() - Random();

			pOut[i ^ bSwap] = Quantize(fValue);
		}
	}

private:
	// Uniform in [0, 1)
	float Random()
	{
		// xorshift32
		uint32_t x = m_nSeed;
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		m_nSeed = x;
		return static_cast<float>(x >> 8) * (1.0f / 16777216.0f);
	}

	// fValue in steps; rounds to the nearest step and returns 24-bit units
	int32_t Quantize(float fValue) const
	{
		const float fRounded = fValue >= 0.0f ? static_cast<float>(static_cast<int32_t>(fValue + 0.5f))
		                                      : -static_cast<float>(static_cast<int32_t>(-fValue + 0.5f));
		const float fMax = static_cast<float>(Max24 / m_nStep);
		const float fMin = static_cast<float>(Min24 / m_nStep);
		const float fClamped = fRounded > fMax ? fMax : (fRounded < fMin ? fMin : fRounded);
		return static_cast<int32_t>(fClamped) * m_nStep;
	}

	static int32_t Clamp(float fValue)
	{
		if (fValue >= static_cast<float>(Max24))
			return Max24;
		if (fValue <= static_cast<float>(Min24))
			return Min24;
		return static_cast<int32_t>(fValue);
	}

	unsigned m_nBits;
	int32_t m_nStep;
	float m_fStep;
	float m_fSilence;
	size_t m_nSilenceLimit;
	size_t m_nSilentFrames;
	uint32_t m_nSeed;
};

#endif
