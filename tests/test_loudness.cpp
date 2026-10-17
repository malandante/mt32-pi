// Host test for include/loudness.h (the [golem] loudness limiter).
// Build: g++ -std=c++17 -Wall -Wextra -Werror -Iinclude tests/test_loudness.cpp -o test_loudness
//
//   test_loudness                       the checks
//   test_loudness IN.wav OUT.wav GAIN   applies it to a 16-bit stereo WAV

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "loudness.h"

using Loudness::CLimiter;

static int g_nFailures = 0;

static void Check(bool bOk, const char* pName)
{
	std::printf("%s %s\n", bOk ? "PASS" : "FAIL", pName);
	g_nFailures += !bOk;
}

constexpr unsigned Rate = 48000;

// Stereo sine, amplitudes per channel
static std::vector<float> Sine(size_t nFrames, float nLeft, float nRight, float nHz = 440.0f)
{
	std::vector<float> v(nFrames * 2);
	for (size_t i = 0; i < nFrames; ++i)
	{
		const float s = std::sin(2.0f * 3.14159265f * nHz * i / Rate);
		v[i * 2]     = nLeft * s;
		v[i * 2 + 1] = nRight * s;
	}
	return v;
}

static void Run(CLimiter& Limiter, std::vector<float>& v)
{
	for (size_t i = 0; i < v.size() / 2; i += 256)
		Limiter.Process(v.data() + i * 2, std::min<size_t>(256, v.size() / 2 - i));
}

static float Peak(const std::vector<float>& v, size_t nFrom = 0, size_t nTo = SIZE_MAX)
{
	float p = 0;
	for (size_t i = nFrom * 2; i < std::min(nTo * 2, v.size()); ++i)
		p = std::fmax(p, std::fabs(v[i]));
	return p;
}

static int Checks()
{
	// 1. Below the threshold: just the gain, delayed by the look-ahead
	{
		CLimiter Limiter;
		Limiter.Configure(Rate, 2.0f);
		const std::vector<float> In = Sine(4800, 0.1f, 0.1f);
		std::vector<float> Out = In;
		Run(Limiter, Out);
		bool bExact = true;
		for (size_t i = CLimiter::Lookahead * 2; i < Out.size(); ++i)
			bExact = bExact && Out[i] == In[i - CLimiter::Lookahead * 2] * 2.0f;
		Check(bExact, "quiet signal: gain only, 64 frames late");
	}

	// 2. Far above: limited to the threshold, not clipped
	{
		CLimiter Limiter;
		Limiter.Configure(Rate, 3.0f);
		std::vector<float> v = Sine(Rate, 0.8f, 0.8f);	// 2.4 after the gain
		Run(Limiter, v);
		const float nSettled = Peak(v, Rate / 2);
		size_t nKnee = 0;
		for (size_t i = Rate; i < v.size(); ++i)
			nKnee += std::fabs(v[i]) > CLimiter::KneeStart;
		Check(Peak(v) <= 1.0f, "never above full scale");
		Check(std::fabs(nSettled - CLimiter::Threshold) < 0.02f, "loud tone settles at the threshold");
		Check(nKnee == 0, "the soft clip does not have to act on a steady tone");
	}

	// 3. A sudden peak after silence: the look-ahead catches it
	{
		CLimiter Limiter;
		Limiter.Configure(Rate, 4.0f);
		std::vector<float> v(Rate * 2, 0.0f);
		const std::vector<float> Burst = Sine(Rate / 10, 0.9f, 0.9f, 1000.0f);
		std::copy(Burst.begin(), Burst.end(), v.begin() + Rate);	// at 0.5 s
		Run(Limiter, v);
		Check(Peak(v) <= CLimiter::KneeStart, "sudden peak caught before the soft clip");
	}

	// 4. Release: the gain comes back after a loud passage
	{
		CLimiter Limiter;
		Limiter.Configure(Rate, 2.0f);
		std::vector<float> Loud = Sine(Rate / 2, 1.0f, 1.0f);
		Run(Limiter, Loud);
		const float nDown = Limiter.GetEnvelope();
		std::vector<float> Quiet = Sine(Rate, 0.1f, 0.1f);
		Run(Limiter, Quiet);
		Check(nDown < 0.5f && Limiter.GetEnvelope() > 0.99f, "gain back within a second");
	}

	// 5. Stereo-linked: a loud left lowers the right by the same factor
	{
		CLimiter Limiter;
		Limiter.Configure(Rate, 3.0f);
		std::vector<float> v = Sine(Rate, 0.9f, 0.1f);
		Run(Limiter, v);
		const float nL = Peak(v, Rate / 2);
		float pR = 0;
		for (size_t i = Rate / 2; i < Rate; ++i)
			pR = std::fmax(pR, std::fabs(v[i * 2 + 1]));
		Check(std::fabs(pR / nL - 0.1f / 0.9f) < 0.01f, "stereo image kept");
	}

	// 6. Gain below 1 is taken as 1 (never quieter than without it)
	{
		CLimiter Limiter;
		Limiter.Configure(Rate, 0.5f);
		Check(Limiter.GetGain() == 1.0f, "gain at least 1");
	}

	std::printf("%s\n", g_nFailures ? "FAILED" : "all passed");
	return g_nFailures ? 1 : 0;
}

// Minimal 16-bit stereo WAV in and out (44-byte header)
static int ProcessFile(const char* pIn, const char* pOut, float nGain)
{
	std::FILE* f = std::fopen(pIn, "rb");
	if (!f)
		return 2;
	uint8_t Header[44];
	if (std::fread(Header, 1, 44, f) != 44)
		return 2;
	std::vector<int16_t> s;
	int16_t x;
	while (std::fread(&x, 2, 1, f) == 1)
		s.push_back(x);
	std::fclose(f);

	std::vector<float> v(s.size());
	for (size_t i = 0; i < s.size(); ++i)
		v[i] = s[i] / 32768.0f;
	CLimiter Limiter;
	uint32_t nRate;
	std::memcpy(&nRate, Header + 24, 4);
	Limiter.Configure(nRate, nGain);
	Run(Limiter, v);

	f = std::fopen(pOut, "wb");
	std::fwrite(Header, 1, 44, f);
	for (float y : v)
	{
		const int16_t o = static_cast<int16_t>(std::lrint(y * 32767.0f));
		std::fwrite(&o, 2, 1, f);
	}
	std::fclose(f);
	return 0;
}

int main(int argc, char** argv)
{
	if (argc == 4)
		return ProcessFile(argv[1], argv[2], static_cast<float>(std::atof(argv[3])));
	return Checks();
}
