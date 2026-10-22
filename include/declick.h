//
// declick.h
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
//
// A short fade around a cut the synth makes on its own: the MT-32 reset SysEx
// (Munt stops its reverb tail at once) or a new OPL3 player. Without it the
// sound jumps from the tail to nothing in one sample: a click, louder with
// the loudness gain (seen in a capture of 2026-10-21).
//
// The MIDI side asks for the fade-out and waits until the render has reached
// silence, makes the cut, and asks for the fade-in. The messages behind the
// cut wait meanwhile, so their order is kept. Kept free of Circle so the host
// tests use the same code (tests/test_declick.cpp).
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

#ifndef _declick_h
#define _declick_h

#include <stddef.h>

class CDeclick
{
public:
	// 5 ms at 48 kHz: too short to hear as a fade
	static constexpr unsigned FadeFrames = 240;

	enum TState : int
	{
		Open,		// passes the sound unchanged
		FadingOut,
		Silent,		// waiting for the cut
		FadingIn,
	};

	// MIDI side
	void RequestFadeOut() { __atomic_store_n(&m_nState, FadingOut, __ATOMIC_RELEASE); }
	bool IsSilent() const { return __atomic_load_n(&m_nState, __ATOMIC_ACQUIRE) == Silent; }
	void RequestFadeIn() { __atomic_store_n(&m_nState, FadingIn, __ATOMIC_RELEASE); }

	// Render side: interleaved stereo, in place
	void Process(float* pSamples, size_t nFrames)
	{
		int nState = __atomic_load_n(&m_nState, __ATOMIC_ACQUIRE);
		if (nState == Open)
			return;

		constexpr float nStep = 1.0f / FadeFrames;
		for (size_t i = 0; i < nFrames; ++i)
		{
			if (nState == FadingOut)
			{
				m_nGain -= nStep;
				if (m_nGain <= 0.0f)
				{
					m_nGain = 0.0f;
					nState = Silent;
					// A fade-in asked meanwhile wins: keep it
					int nExpected = FadingOut;
					__atomic_compare_exchange_n(&m_nState, &nExpected, Silent, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
					nState = nExpected == FadingOut ? Silent : nExpected;
				}
			}
			else if (nState == FadingIn)
			{
				m_nGain += nStep;
				if (m_nGain >= 1.0f)
				{
					m_nGain = 1.0f;
					int nExpected = FadingIn;
					__atomic_compare_exchange_n(&m_nState, &nExpected, Open, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
					nState = nExpected == FadingIn ? Open : nExpected;
				}
			}
			pSamples[i * 2] *= m_nGain;
			pSamples[i * 2 + 1] *= m_nGain;
		}
	}

	float GetGain() const { return m_nGain; }

private:
	int m_nState = Open;
	float m_nGain = 1.0f;
};

#endif
