// Host test for include/nextaudiomix.h: the Next's audio received on I2S.
// Build: g++ -std=c++17 -Wall -Wextra -Werror -Iinclude tests/test_nextaudiomix.cpp -o test_nextaudiomix
//
// Part 1 models the wire bit by bit: the Next core's i2s_slave and
// i2s_transmit (src/audio/i2s/*.vhd in the ZX Next FPGA sources, 3.01.09) as
// written, against the Pi's PCM receiver as Circle sets it up (channel 1 at
// bit position 1 after the frame sync edge, 24 bits wide, sign-extended).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "nextaudiomix.h"

static int g_nFailures = 0;

static void Check(bool bOk, const char* pName)
{
	std::printf("%s %s\n", bOk ? "PASS" : "FAIL", pName);
	g_nFailures += !bOk;
}

// The Next sends the mixer's unsigned 13-bit value with the top bit inverted
// (i2s.vhd: "(not i_audio_zxn_L(12)) & i_audio_zxn_L(11 downto 0)")
static uint16_t NextWire(uint16_t nUnsigned)
{
	return static_cast<uint16_t>((nUnsigned ^ 0x1000) & 0x1FFF);
}

// One stereo frame through the link. Pi is master: WS changes on SCK falling
// edges, low = left, 32 SCK periods per channel. Returns the two received
// 24-bit slots, sign-extended to 32 bits.
static void Wire(uint16_t nLeft, uint16_t nRight, int32_t& nGotLeft, int32_t& nGotRight)
{
	constexpr int Width = 13, ChanLen = 32;
	const uint16_t L = NextWire(nLeft), R = NextWire(nRight);

	// Two frames, so the transmitter is loaded the way it is in a stream;
	// the second frame is the one read
	std::vector<int> Ws;
	for (int f = 0; f < 3; ++f)
		for (int i = 0; i < 2 * ChanLen; ++i)
			Ws.push_back(i < ChanLen ? 0 : 1);

	// i2s_slave: wsp = WS changed, registered on the SCK rising edge
	// i2s_transmit: on the SCK falling edge, load L (ws = 0) or R when wsp,
	// otherwise shift left; the data line is the top bit
	int ws_d = 1, wsp = 0;
	uint16_t nShift = 1 << (Width - 1);
	std::vector<int> Sampled;	// data seen by the Pi on each rising edge
	for (size_t t = 0; t < Ws.size(); ++t)
	{
		const int ws = Ws[t];	// WS for this SCK period (set on the falling edge before)
		// rising edge: the Pi samples SD; the slave registers ws
		Sampled.push_back((nShift >> (Width - 1)) & 1);
		wsp  = ws != ws_d;
		ws_d = ws;
		// falling edge: the transmitter acts
		if (wsp)
			nShift = ws == 0 ? L : R;
		else
			nShift = static_cast<uint16_t>((nShift << 1) & ((1 << Width) - 1));
	}

	// The Pi: channel 1 (left) starts one bit after the frame sync edge
	// (CH1POS = 1), channel 2 at CHANLEN + 1; 24 bits each, sign-extended
	auto Read = [&](size_t nStart) {
		int32_t v = 0;
		for (int b = 0; b < 24; ++b)
			v = v << 1 | Sampled[nStart + b];
		return (v << 8) >> 8;
	};
	const size_t nFrame = 2 * ChanLen * 2;	// third frame
	nGotLeft  = Read(nFrame + 1);
	nGotRight = Read(nFrame + ChanLen + 1);
}

int main()
{
	// 1. Wire alignment: the 13-bit value arrives shifted left by 11
	{
		bool bOk = true;
		for (uint16_t u : {0, 1, 1536, 4095, 4096, 4097, 5998, 8191})
		{
			int32_t l, r;
			Wire(u, static_cast<uint16_t>(8191 - u), l, r);
			const int32_t WantL = (static_cast<int32_t>(u) - 4096) * 2048;
			const int32_t WantR = (static_cast<int32_t>(8191 - u) - 4096) * 2048;
			if (l != WantL || r != WantR)
			{
				std::printf("  u=%u: got %d %d, want %d %d\n", u, l, r, WantL, WantR);
				bOk = false;
			}
		}
		Check(bOk, "13-bit Next samples land at bit 23..11 of the 24-bit slot");
		Check(NextAudio::WordToSample(static_cast<uint32_t>(-4096 * 2048)) == -1.0f &&
		      std::fabs(NextAudio::WordToSample(4095u * 2048) - 4095.0f / 4096) < 1e-6f,
		      "full scale is -1..1");
	}

	// 2. DC blocker: the mixer's resting level goes away, a tone stays
	{
		NextAudio::TDCBlocker DC;
		const float nRest = NextAudio::WordToSample(static_cast<uint32_t>((1536 - 4096) * 2048));
		float nFirst = DC.Process(nRest), nLast = 0;
		for (int i = 1; i < 48000; ++i)
			nLast = DC.Process(nRest);
		Check(nFirst == 0.0f && std::fabs(nLast) < 1e-3f, "resting level removed, no thump at start");

		float nPeak = 0;
		for (int i = 0; i < 48000; ++i)
		{
			const float x = nRest + ((i / 24) % 2 ? 0.25f : -0.25f);	// 1 kHz square, 0.25
			const float y = DC.Process(x);
			if (i > 24000)
				nPeak = std::fmax(nPeak, std::fabs(y));
		}
		Check(nPeak > 0.24f && nPeak < 0.27f, "1 kHz passes at its level");
	}

	// 3. Ring: start at half, silence when dry, drop when full, clip
	{
		NextAudio::CRing<1024> Ring;
		// +-0.5 alternating frames: a tone at half the sample rate, which
		// the DC blocker lets through
		std::vector<uint32_t> Words(2 * 600);
		for (size_t i = 0; i < Words.size(); ++i)
			Words[i] = static_cast<uint32_t>((i / 2) % 2 ? 2048 * 2048 : -2048 * 2048);
		std::vector<float> Out(2 * 256, 0.0f);

		Ring.Put(Words.data(), 400);
		Ring.MixInto(Out.data(), 256, 1.0f);
		Check(Out[0] == 0.0f && Out[511] == 0.0f, "waits until half full");

		Ring.Put(Words.data(), 200);
		Ring.MixInto(Out.data(), 256, 1.0f);
		Check(Out[0] == 0.0f && std::fabs(Out[2]) > 0.9f, "then plays (the first sample primes the DC blocker)");

		// 344 left: the next 256 play, the following call runs dry
		std::fill(Out.begin(), Out.end(), 0.0f);
		Ring.MixInto(Out.data(), 256, 1.0f);
		Ring.MixInto(Out.data(), 256, 1.0f);
		Check(Ring.Underruns() == 1 && Ring.Level() == 0, "runs dry once and waits again");

		Ring.Put(Words.data(), 600);
		Ring.Put(Words.data(), 600);
		Check(Ring.Overruns() == 176 && Ring.Level() == 1024, "a full ring drops what does not fit");

		std::fill(Out.begin(), Out.end(), 0.9f);
		Ring.MixInto(Out.data(), 256, 1.0f);
		bool bInRange = true;
		for (float x : Out)
			bInRange = bInRange && x <= 1.0f && x >= -1.0f;
		Check(bInRange && (Out[0] == 1.0f || Out[2] == 1.0f), "synth plus Next is clipped to -1..1");
	}

	// 4. A floating GPIO 20 (the Next's I2S off): random bits come out silent
	{
		NextAudio::CRing<1024> Ring;
		std::vector<uint32_t> Noise(2 * 128);
		uint32_t x = 12345;
		for (auto& w : Noise)
		{
			x = x * 1103515245u + 12345u;
			w = x;
		}
		for (int i = 0; i < 8; ++i)
			Ring.Put(Noise.data(), 128);
		std::vector<float> Out(2 * 512, 0.0f);
		Ring.MixInto(Out.data(), 512, 1.0f);
		bool bSilent = true;
		for (float v : Out)
			bSilent = bSilent && v == 0.0f;
		Check(bSilent && Ring.InvalidChunks() == 8, "floating input gives silence");

		// The Next's real samples (low 11 bits zero) still pass afterwards
		Ring.MixInto(Out.data(), 512, 1.0f);	// the rest of the silence
		std::vector<uint32_t> Good(2 * 128);
		for (size_t i = 0; i < Good.size(); ++i)
			Good[i] = static_cast<uint32_t>((i / 2) % 2 ? 2048 * 2048 : -2048 * 2048);
		for (int i = 0; i < 8; ++i)
			Ring.Put(Good.data(), 128);
		Ring.MixInto(Out.data(), 512, 1.0f);
		float nPeak = 0;
		for (float v : Out)
			nPeak = std::fmax(nPeak, std::fabs(v));
		Check(nPeak > 0.9f && Ring.InvalidChunks() == 8, "the Next's samples pass again");
	}

	std::printf("%s\n", g_nFailures ? "FAILED" : "all passed");
	return g_nFailures ? 1 : 0;
}
