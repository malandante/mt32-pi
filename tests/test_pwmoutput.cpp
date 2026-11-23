// Host test for include/pwmoutput.h (the conditioned PWM output).
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -Iinclude tests/test_pwmoutput.cpp -o test_pwmoutput

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

#include "pwmoutput.h"

using namespace PWMOutput;

static int g_nFailures = 0;

static void Check(bool bOk, const char* pName)
{
	std::printf("%s %s\n", bOk ? "PASS" : "FAIL", pName);
	if (!bOk)
		++g_nFailures;
}

// Power (dB, relative to full-scale sine) of a signal between two frequencies
static double BandPower(const std::vector<double>& x, double nRate, double nLow, double nHigh)
{
	const size_t n = x.size();
	double nTotal = 0.0;
	const double nStep = nRate / n;
	for (double f = nLow; f < nHigh; f += nStep)
	{
		std::complex<double> s = 0;
		for (size_t i = 0; i < n; ++i)
		{
			const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / n);	// Hann
			s += x[i] * w * std::polar(1.0, -2 * M_PI * f * i / nRate);
		}
		nTotal += std::norm(s);
	}
	return 10 * std::log10(nTotal / (n * n * 0.375 / 4 * 0.5) + 1e-30);
}

// The analogue level of each output sample, -1..1, from what Circle makes of it
static std::vector<double> Analogue(const std::vector<int32_t>& Out, uint32_t nRangeMax, int nChannel)
{
	std::vector<double> x;
	for (size_t i = nChannel; i < Out.size(); i += 2)
		x.push_back((CircleLevel(Out[i], nRangeMax) + 0.5) / (nRangeMax * 0.5) - 1.0);
	return x;
}

int main()
{
	// Pi 4: 125 MHz, 48 and 96 kHz
	const uint32_t nRange48 = 2604 - 1, nRange96 = 1302 - 1;

	// Every level reaches Circle exactly
	{
		bool bExact = true;
		for (uint32_t r : {nRange48, nRange96, 5208u - 1})
			for (uint32_t l = 0; l < r; ++l)
				bExact = bExact && CircleLevel(SampleForLevel(l, r), r) == l;
		Check(bExact, "each level, as a 24-bit sample, is that level after Circle's conversion");
	}

	// Without dither it is exactly what Circle did before (truncation)
	{
		CQuantizer Q;
		Q.Configure(nRange48, TDither::Off, 48000);
		std::vector<float> In;
		for (int i = 0; i < 4800; ++i)
			In.push_back(0.3f * std::sin(i * 0.01f)), In.push_back(-0.7f * std::cos(i * 0.013f));
		std::vector<int32_t> Out(In.size());
		Q.Process(In.data(), In.size() / 2, Out.data());
		bool bSame = true;
		for (size_t i = 0; i < In.size(); ++i)
		{
			const int32_t nOld = static_cast<int32_t>(In[i] * ((1 << 23) - 1));
			const int nDiff = static_cast<int>(CircleLevel(Out[i], nRange48)) - static_cast<int>(CircleLevel(nOld, nRange48));
			bSame = bSame && nDiff >= -1 && nDiff <= 1;
		}
		Check(bSame, "dither off: the same levels as Circle's own conversion (within one)");
	}

	// Silence: still, at the mid level Circle uses when idle, even with dither
	{
		CQuantizer Q;
		Q.Configure(nRange48, TDither::Shaped, 48000);
		std::vector<float> In(512, 0.0f);
		std::vector<int32_t> Out(512);
		Q.Process(In.data(), 256, Out.data());
		bool bStill = true;
		for (int32_t s : Out)
			bStill = bStill && CircleLevel(s, nRange48) == nRange48 / 2;
		Check(bStill, "silence stays at the idle level, without dither");
	}

	// Upsampler: flat where it is heard, the image above it removed
	{
		CUpsampler2x U;
		auto Gain = [&](double f) {
			CUpsampler2x V;
			std::vector<float> In, Out;
			for (int i = 0; i < 9600; ++i)
			{
				const float v = 0.5f * std::sin(2 * M_PI * f * i / 48000.0);
				In.push_back(v), In.push_back(v);
			}
			Out.resize(In.size() * 2);
			V.Process(In.data(), 9600, Out.data());
			std::vector<double> x;
			for (size_t i = 4800; i < Out.size() / 2; ++i)
				x.push_back(Out[i * 2]);
			return std::make_pair(BandPower(x, 96000, f - 200, f + 200), BandPower(x, 96000, 48000 - f - 200, 48000 - f + 200));
		};
		const auto g1 = Gain(1000), g15 = Gain(15000), g19 = Gain(19000);
		const double nRef = BandPower([] { std::vector<double> x; for (int i = 0; i < 14400; ++i) x.push_back(0.5 * std::sin(2 * M_PI * 1000 * i / 96000.0)); return x; }(), 96000, 800, 1200);
		std::printf("     1 kHz %.2f dB, 15 kHz %.2f dB, 19 kHz %.2f dB (ref %.2f); images %.1f %.1f %.1f dB\n",
		            g1.first, g15.first, g19.first, nRef, g1.second, g15.second, g19.second);
		Check(std::fabs(g1.first - nRef) < 0.1 && std::fabs(g15.first - nRef) < 0.2 && std::fabs(g19.first - nRef) < 0.5,
		      "upsampler: flat up to 19 kHz");
		Check(g1.second - nRef < -70 && g15.second - nRef < -60, "upsampler: images below -60 dB");
		(void)U;
	}

	// A quiet decaying tone, as a reverb tail: noise and distortion that land
	// in the audible band (20 Hz - 16 kHz) for each setting
	{
		auto Run = [&](bool bOversample, TDither Dither) {
			const double nRate = bOversample ? 96000 : 48000;
			const uint32_t nRange = bOversample ? nRange96 : nRange48;
			CUpsampler2x U;
			CQuantizer Q;
			Q.Configure(nRange, Dither, static_cast<unsigned>(nRate));
			std::vector<float> In;
			std::vector<double> Ideal;
			for (int i = 0; i < 9600; ++i)
			{
				const float v = 0.003f * std::sin(2 * M_PI * 440 * i / 48000.0) + 0.0015f * std::sin(2 * M_PI * 1330 * i / 48000.0);
				In.push_back(v), In.push_back(v);
			}
			std::vector<float> Up(In.size() * 2);
			const float* pIn = In.data();
			size_t nFrames = 9600;
			if (bOversample)
			{
				U.Process(In.data(), 9600, Up.data());
				pIn = Up.data();
				nFrames = 19200;
			}
			std::vector<int32_t> Out(nFrames * 2);
			Q.Process(pIn, nFrames, Out.data());
			std::vector<double> a = Analogue(Out, nRange, 0), err;
			for (size_t i = 0; i < a.size(); ++i)
				err.push_back(a[i] - pIn[i * 2]);
			err.erase(err.begin(), err.begin() + err.size() / 4);
			return BandPower(err, nRate, 20, 16000);
		};
		const double nToday = Run(false, TDither::Off), nDither = Run(false, TDither::On),
		             nOver = Run(true, TDither::On), nShaped = Run(true, TDither::Shaped);
		std::printf("     error in 20 Hz-16 kHz: today %.1f, dither %.1f, x2+dither %.1f, x2+shaped %.1f dB\n",
		            nToday, nDither, nOver, nShaped);
		Check(nShaped < nDither - 6.0, "x2 with shaped dither: at least 6 dB less noise heard than 48 kHz dither");
	}

	// Loud, clipping music: the shaping stays stable (its error is bounded)
	{
		CQuantizer Q;
		Q.Configure(nRange96, TDither::Shaped, 96000);
		std::vector<float> In;
		for (int i = 0; i < 96000; ++i)
		{
			const float v = 1.2f * std::sin(2 * M_PI * 110 * i / 96000.0) * std::sin(2 * M_PI * 0.5 * i / 96000.0);
			In.push_back(v), In.push_back(-v);
		}
		std::vector<int32_t> Out(In.size());
		Q.Process(In.data(), 96000, Out.data());
		const std::vector<double> a = Analogue(Out, nRange96, 0);
		double nWorst = 0.0;
		for (size_t i = 0; i < a.size(); ++i)
		{
			const double x = In[i * 2] > 1 ? 1 : In[i * 2] < -1 ? -1 : In[i * 2];
			nWorst = std::fmax(nWorst, std::fabs(a[i] - x));
		}
		std::printf("     largest deviation with clipping: %.4f (%.1f levels)\n", nWorst, nWorst * nRange96 / 2);
		Check(nWorst < 25.0 / nRange96, "shaped: stable through clipping (within a few levels)");
	}

	std::printf("%s\n", g_nFailures ? "FAILED" : "all passed");
	return g_nFailures ? 1 : 0;
}
