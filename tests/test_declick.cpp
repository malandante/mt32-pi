// Host test for include/declick.h (the fade around a synth's own cut).
// Build: g++ -std=c++17 -Wall -Wextra -Werror -Iinclude tests/test_declick.cpp -o test_declick

#include <cmath>
#include <cstdio>
#include <vector>

#include "declick.h"

static int g_nFailures = 0;

static void Check(bool bOk, const char* pName)
{
	std::printf("%s %s\n", bOk ? "PASS" : "FAIL", pName);
	if (!bOk)
		++g_nFailures;
}

// A loud tone in blocks of 256 frames, as the audio task renders
static void Render(CDeclick& Declick, std::vector<float>& Out, size_t nFrames, float nLevel)
{
	float Block[256 * 2];
	for (size_t nDone = 0; nDone < nFrames;)
	{
		const size_t n = nFrames - nDone < 256 ? nFrames - nDone : 256;
		for (size_t i = 0; i < n; ++i)
			Block[i * 2] = Block[i * 2 + 1] = nLevel * std::sin(0.05f * (Out.size() / 2 + i));
		Declick.Process(Block, n);
		Out.insert(Out.end(), Block, Block + n * 2);
		nDone += n;
	}
}

static float LargestStep(const std::vector<float>& Out)
{
	float nMax = 0.0f;
	for (size_t i = 2; i < Out.size(); i += 2)
		nMax = std::fmax(nMax, std::fabs(Out[i] - Out[i - 2]));
	return nMax;
}

int main()
{
	// Untouched while open
	{
		CDeclick Declick;
		std::vector<float> Out;
		Render(Declick, Out, 1000, 0.5f);
		bool bSame = true;
		for (size_t i = 0; i < 1000; ++i)
			bSame = bSame && Out[i * 2] == 0.5f * std::sin(0.05f * i);
		Check(bSame, "open: the sound passes unchanged");
	}

	// Fade out, silent until asked, fade in
	{
		CDeclick Declick;
		std::vector<float> Out;
		Render(Declick, Out, 1000, 0.5f);
		Declick.RequestFadeOut();
		Check(!Declick.IsSilent(), "not silent before the render fades");
		Render(Declick, Out, CDeclick::FadeFrames + 10, 0.5f);
		Check(Declick.IsSilent(), "silent once the fade is rendered");
		const size_t nCut = Out.size() / 2;

		// The cut: the synth jumps to another sound (here a louder one)
		Render(Declick, Out, 500, 0.9f);
		bool bQuiet = true;
		for (size_t i = nCut; i < Out.size() / 2; ++i)
			bQuiet = bQuiet && Out[i * 2] == 0.0f;
		Check(bQuiet, "silent while waiting for the cut");

		Declick.RequestFadeIn();
		Render(Declick, Out, 1000, 0.9f);
		Check(Declick.GetGain() == 1.0f, "fully open again after the fade-in");

		// No step larger than the tone's own (0.9 x 0.05 per frame) plus the ramp
		Check(LargestStep(Out) < 0.9f * 0.05f + 1.0f / CDeclick::FadeFrames, "no jump anywhere: no click");

		// Without the fade, the same cut jumps
		std::vector<float> Raw;
		CDeclick Open;
		Render(Open, Raw, 1032, 0.5f);
		const float nLast = std::fabs(Raw[Raw.size() - 2]);
		Raw.push_back(0.0f);
		Raw.push_back(0.0f);
		Check(LargestStep(Raw) >= nLast && nLast > 0.1f, "(the same cut without the fade jumps)");
	}

	// A fade-in asked before the fade-out finished takes over smoothly
	{
		CDeclick Declick;
		std::vector<float> Out;
		Render(Declick, Out, 300, 0.5f);
		Declick.RequestFadeOut();
		Render(Declick, Out, CDeclick::FadeFrames / 2, 0.5f);
		Declick.RequestFadeIn();
		Render(Declick, Out, 1000, 0.5f);
		Check(Declick.GetGain() == 1.0f && LargestStep(Out) < 0.5f * 0.05f + 1.0f / CDeclick::FadeFrames,
		      "fade-in during the fade-out: smooth, open at the end");
	}

	std::printf("%s\n", g_nFailures ? "FAILED" : "all passed");
	return g_nFailures ? 1 : 0;
}
