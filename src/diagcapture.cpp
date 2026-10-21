//
// diagcapture.cpp
//

#include <circle/logger.h>
#include <circle/string.h>
#include <fatfs/ff.h>
#include <string.h>

#include "diagcapture.h"

LOGMODULE("capture");

namespace DiagCapture
{
	constexpr unsigned Seconds = 60;
	constexpr unsigned MaxEvents = 32768;

	struct TEvent
	{
		u32 nFrame;
		u16 nLength;	// 0: short message
		u8 Data[14];	// short message, or the first bytes of a SysEx
	};

	static s16* s_pAudio = nullptr;	// 4 channels per frame
	static unsigned s_nFrames = 0;		// ring size
	static unsigned s_nSampleRate = 48000;
	static volatile u32 s_nFrame = 0;	// frames written in total
	static volatile bool s_bFrozen = false;
	static TEvent* s_Events = nullptr;	// on the heap: 650 KB in BSS overran the kernel's 2 MB
	static unsigned s_nEvents = 0;

	static s16 ToS16(float x)
	{
		if (x > 1.0f) x = 1.0f;
		if (x < -1.0f) x = -1.0f;
		return static_cast<s16>(x * 32767.0f);
	}

	void Init(unsigned nSampleRate)
	{
		s_nSampleRate = nSampleRate;
		s_nFrames = nSampleRate * Seconds;
		s_Events = new TEvent[MaxEvents];
		s16* pAudio = new s16[s_nFrames * 4];
		memset(pAudio, 0, s_nFrames * 4 * sizeof(s16));
		s_pAudio = pAudio;
		LOGNOTE("Audio capture: %u s ready", Seconds);
	}

	void Audio(const float* pBuffer, size_t nFrames, bool bOutput)
	{
		if (!s_pAudio || s_bFrozen)
			return;
		const u32 nStart = s_nFrame;
		for (size_t i = 0; i < nFrames; ++i)
		{
			s16* pFrame = s_pAudio + ((nStart + i) % s_nFrames) * 4 + (bOutput ? 2 : 0);
			pFrame[0] = ToS16(pBuffer[i * 2]);
			pFrame[1] = ToS16(pBuffer[i * 2 + 1]);
		}
		if (bOutput)
			s_nFrame = nStart + nFrames;
	}

	void ShortMessage(u32 nMessage)
	{
		if (!s_Events || s_bFrozen || (nMessage & 0xFF) >= 0xF8)	// no clock or active sensing
			return;
		TEvent& Event = s_Events[s_nEvents++ % MaxEvents];
		Event.nFrame = s_nFrame;
		Event.nLength = 0;
		Event.Data[0] = nMessage & 0xFF;
		Event.Data[1] = (nMessage >> 8) & 0xFF;
		Event.Data[2] = (nMessage >> 16) & 0xFF;
	}

	static void PutLE(u8* p, u32 v, int n)
	{
		for (int i = 0; i < n; ++i)
			p[i] = (v >> (8 * i)) & 0xFF;
	}

	static void Write()
	{
		if (!s_pAudio)
			return;
		s_bFrozen = true;
		const u32 nEnd = s_nFrame;
		const u32 nCount = nEnd < s_nFrames ? nEnd : s_nFrames;
		const u32 nFirst = nEnd - nCount;

		FIL File;
		UINT nWritten;
		if (f_open(&File, "SD:capture.wav", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK)
		{
			u8 Header[44];
			memcpy(Header, "RIFF", 4); PutLE(Header + 4, 36 + nCount * 8, 4);
			memcpy(Header + 8, "WAVEfmt ", 8); PutLE(Header + 16, 16, 4);
			PutLE(Header + 20, 1, 2); PutLE(Header + 22, 4, 2);
			PutLE(Header + 24, s_nSampleRate, 4); PutLE(Header + 28, s_nSampleRate * 8, 4);
			PutLE(Header + 32, 8, 2); PutLE(Header + 34, 16, 2);
			memcpy(Header + 36, "data", 4); PutLE(Header + 40, nCount * 8, 4);
			f_write(&File, Header, sizeof(Header), &nWritten);
			// The ring in up to two pieces, oldest first
			const u32 nStart = nFirst % s_nFrames;
			const u32 nFirstPart = nCount < s_nFrames - nStart ? nCount : s_nFrames - nStart;
			f_write(&File, s_pAudio + nStart * 4, nFirstPart * 8, &nWritten);
			if (nCount > nFirstPart)
				f_write(&File, s_pAudio, (nCount - nFirstPart) * 8, &nWritten);
			f_close(&File);
		}

		if (f_open(&File, "SD:capture.txt", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK)
		{
			CString Line;
			Line.Format("# capture: %u Hz, %u frames in capture.wav; frame = position in it (negative: before)\n",
			            s_nSampleRate, static_cast<unsigned>(nCount));
			f_write(&File, static_cast<const char*>(Line), Line.GetLength(), &nWritten);
			const unsigned nShown = s_nEvents < MaxEvents ? s_nEvents : MaxEvents;
			for (unsigned i = s_nEvents - nShown; i < s_nEvents; ++i)
			{
				const TEvent& Event = s_Events[i % MaxEvents];
				const int nFrame = static_cast<int>(Event.nFrame - nFirst);
				if (Event.nLength == 0)
					Line.Format("%d %02x %02x %02x\n", nFrame, Event.Data[0], Event.Data[1], Event.Data[2]);
				else
				{
					Line.Format("%d sysex %u:", nFrame, Event.nLength);
					for (unsigned j = 0; j < Event.nLength && j < sizeof(Event.Data); ++j)
					{
						CString Byte;
						Byte.Format(" %02x", Event.Data[j]);
						Line.Append(Byte);
					}
					Line.Append("\n");
				}
				f_write(&File, static_cast<const char*>(Line), Line.GetLength(), &nWritten);
			}
			f_close(&File);
		}

		LOGNOTE("Capture written: %u frames, %u events", static_cast<unsigned>(nCount), s_nEvents);
		s_nEvents = 0;
		s_bFrozen = false;
	}

	bool SysEx(const u8* pData, size_t nSize)
	{
		static const u8 GMSystemOn[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
		if (nSize == sizeof(GMSystemOn) && memcmp(pData, GMSystemOn, nSize) == 0)
		{
			Write();
			return true;
		}
		if (!s_Events || s_bFrozen)
			return false;
		TEvent& Event = s_Events[s_nEvents++ % MaxEvents];
		Event.nFrame = s_nFrame;
		Event.nLength = static_cast<u16>(nSize);
		memcpy(Event.Data, pData, nSize < sizeof(Event.Data) ? nSize : sizeof(Event.Data));
		return false;
	}
}
