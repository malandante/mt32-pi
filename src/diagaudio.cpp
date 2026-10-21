//
// diagaudio.cpp
//

#include <circle/timer.h>

#include "diagaudio.h"

namespace DiagAudio
{
	TWindow Windows[WindowCount];
	volatile unsigned nWindows = 0;

	static TWindow Current;
	static float nLastSynth = 0.0f, nLastOutput = 0.0f;
	static unsigned nFramesInWindow = 0;

	void Measure(const float* pBuffer, size_t nFrames, bool bOutput, unsigned nSampleRate)
	{
		float nPeak = bOutput ? Current.nOutputPeak : Current.nSynthPeak;
		u32 nCrossings = bOutput ? Current.nOutputCrossings : Current.nSynthCrossings;
		float& nLast = bOutput ? nLastOutput : nLastSynth;

		for (size_t i = 0; i < nFrames * 2; ++i)
		{
			const float nSample = pBuffer[i];
			const float nMagnitude = nSample < 0.0f ? -nSample : nSample;
			if (nMagnitude > nPeak)
				nPeak = nMagnitude;
			if ((i & 1) == 0)
			{
				if ((nSample > 0.0f) != (nLast > 0.0f))
					++nCrossings;
				nLast = nSample;
			}
		}

		if (!bOutput)
		{
			Current.nSynthPeak = nPeak;
			Current.nSynthCrossings = nCrossings;
			return;
		}

		Current.nOutputPeak = nPeak;
		Current.nOutputCrossings = nCrossings;
		nFramesInWindow += nFrames;
		if (nFramesInWindow >= nSampleRate)
		{
			Current.nTime = CTimer::GetClockTicks();
			Windows[nWindows % WindowCount] = Current;
			++nWindows;
			Current = TWindow();
			nFramesInWindow = 0;
		}
	}
}
