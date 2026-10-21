//
// diagcapture.h
//
// Diagnostic build only (diag/audio-capture): the last 60 s of audio, the
// synth's own render and the final output, and every MIDI message with the
// audio frame it arrived at. GM System On (.golem reset) writes them to the
// SD card as capture.wav (4 channels: synth L, R, output L, R) and
// capture.txt, before the reset is applied.
//

#ifndef _diagcapture_h
#define _diagcapture_h

#include <circle/types.h>
#include <stddef.h>

namespace DiagCapture
{
	void Init(unsigned nSampleRate);
	// bOutput false: the synth's render; true: after loudness and the Next's
	// input (this one advances the frame count)
	void Audio(const float* pBuffer, size_t nFrames, bool bOutput);
	void ShortMessage(u32 nMessage);
	// True when the message is GM System On: the capture has been written
	bool SysEx(const u8* pData, size_t nSize);
}

#endif
