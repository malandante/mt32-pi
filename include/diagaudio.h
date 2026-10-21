//
// diagaudio.h
//
// Diagnostic build only (diag/opl3-capture): one record per second of audio,
// the synth's own render and the final output (after loudness and the Next's
// input), so a capture shows where a residual sound comes from.
//

#ifndef _diagaudio_h
#define _diagaudio_h

#include <circle/types.h>
#include <stddef.h>

namespace DiagAudio
{
	struct TWindow
	{
		u32 nTime;		// microseconds at the end of the second
		float nSynthPeak;	// synth render, before loudness and the Next's input
		float nOutputPeak;	// what goes to the DAC
		u32 nSynthCrossings;	// zero crossings, left channel
		u32 nOutputCrossings;
	};

	constexpr unsigned WindowCount = 600;
	extern TWindow Windows[WindowCount];
	extern volatile unsigned nWindows;

	// Called by the audio task with each buffer, before (bOutput false) and
	// after (true) loudness and the Next's input
	void Measure(const float* pBuffer, size_t nFrames, bool bOutput, unsigned nSampleRate);
}

#endif
