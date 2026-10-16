// Host unit test for include/dither.h
// Build: g++ -std=c++17 -Wall -Wextra -Werror -Iinclude tests/test_dither.cpp -o test_dither

#include <cmath>
#include <cstdio>
#include <vector>

#include "dither.h"

static int g_nFailures = 0;

static void Check(bool bOK, const char* pName)
{
	std::printf("%s %s\n", bOK ? "PASS" : "FAIL", pName);
	g_nFailures += !bOK;
}

int main()
{
	constexpr size_t nFrames = 256;
	std::vector<float> In(nFrames * 2);
	std::vector<int32_t> Out(nFrames * 2);

	// Off: plain conversion, clamped, channels swapped on request
	{
		CDither Dither(0);
		In[0] = 0.5f; In[1] = -0.25f; In[2] = 2.0f; In[3] = -2.0f;
		Dither.Convert(In.data(), Out.data(), 2, false);
		Check(Out[0] == static_cast<int32_t>(0.5f * CDither::Max24) && Out[2] == CDither::Max24 && Out[3] == CDither::Min24,
		      "off: plain 24-bit conversion with clamping");
		Dither.Convert(In.data(), Out.data(), 2, true);
		Check(Out[1] == static_cast<int32_t>(0.5f * CDither::Max24) && Out[0] == static_cast<int32_t>(-0.25f * CDither::Max24),
		      "off: reversed stereo");
	}

	// 10 bits: every sample a multiple of the step, within the 10-bit range
	{
		CDither Dither(10, 4800);
		Check(Dither.GetStep() == 1 << 14, "10 bits: step is 2^14 in 24-bit units");
		bool bOK = true;
		for (int nBlock = 0; nBlock < 50; ++nBlock)
		{
			for (size_t i = 0; i < In.size(); ++i)
				In[i] = 1.5f * std::sin(0.01f * (nBlock * In.size() + i));
			Dither.Convert(In.data(), Out.data(), nFrames, false);
			for (int32_t n : Out)
				bOK &= n % (1 << 14) == 0 && n >= -512 * (1 << 14) && n <= 511 * (1 << 14);
		}
		Check(bOK, "10 bits: output on the 10-bit grid, clamped");
	}

	// A quiet constant (a tenth of a step) keeps its average through the dither
	{
		CDither Dither(10, 4800);
		const float fLevel = 0.1f * (1 << 14) / CDither::Max24;
		double fSum = 0;
		size_t nCount = 0;
		for (int nBlock = 0; nBlock < 400; ++nBlock)
		{
			for (float& f : In) f = fLevel;
			Dither.Convert(In.data(), Out.data(), nFrames, false);
			for (int32_t n : Out) { fSum += n; ++nCount; }
		}
		const double fMean = fSum / nCount / (1 << 14);
		Check(std::fabs(fMean - 0.1) < 0.02, "dither: sub-step level kept on average (truncation would give 0)");
	}

	// Silence: dither continues for the hold time, then exact zeros
	{
		CDither Dither(10, 1024);
		for (float& f : In) f = 0.5f;
		Dither.Convert(In.data(), Out.data(), nFrames, false);   // sound
		for (float& f : In) f = 0.0f;
		bool bNoisy = false;
		for (int nBlock = 0; nBlock < 3; ++nBlock)                 // 768 frames < 1024
		{
			Dither.Convert(In.data(), Out.data(), nFrames, false);
			for (int32_t n : Out) bNoisy |= n != 0;
		}
		Check(bNoisy, "silence: dither held for a moment (no gate on short gaps)");
		bool bZero = true;
		for (int nBlock = 0; nBlock < 4; ++nBlock)
		{
			Dither.Convert(In.data(), Out.data(), nFrames, false);
			if (nBlock >= 1)
				for (int32_t n : Out) bZero &= n == 0;
		}
		Check(bZero, "silence: exact zeros after the hold time");
		for (float& f : In) f = 0.0f;
		In[7] = 0.01f;
		Dither.Convert(In.data(), Out.data(), nFrames, false);
		bool bBack = false;
		for (size_t i = 0; i < Out.size(); ++i) bBack |= Out[i] != 0 && i != 7;
		Check(bBack, "sound again: dither resumes at once");
	}

	std::printf(g_nFailures ? "%d FAILED\n" : "all passed\n", g_nFailures);
	return g_nFailures ? 1 : 0;
}
