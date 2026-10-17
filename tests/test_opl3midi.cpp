// Host test for include/synth/opl3midi.h with libADLMIDI built as the kernel
// builds it (Makefile target adlmidi, without the cross toolchain).
//
//   test_opl3midi                 the checks
//   test_opl3midi EVENTS OUT.WAV [BANK [EMULATOR]]
//                                 renders a list of "ms b1 b2 b3" lines (from
//                                 tests/smf_events.py) to a 48 kHz WAV, through
//                                 the same calls and gain as the kernel

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "synth/opl3midi.h"

static int g_nFailures = 0;

static void Check(bool bOk, const char* pName)
{
	std::printf("%s %s\n", bOk ? "PASS" : "FAIL", pName);
	g_nFailures += !bOk;
}

constexpr unsigned SampleRate = 48000;
constexpr float Gain = 2.5f;	// [opl3] gain default

static ADL_MIDIPlayer* NewPlayer(int nBank, int nEmulator = ADLMIDI_EMU_NUKED)
{
	ADL_MIDIPlayer* p = adl_init(SampleRate);
	adl_switchEmulator(p, nEmulator);
	adl_setNumChips(p, 1);
	adl_setSoftPanEnabled(p, 1);
	adl_setBank(p, nBank);
	return p;
}

// Peak over nMs milliseconds
static float Run(ADL_MIDIPlayer* p, unsigned nMs, float nGain = Gain, bool* pInRange = nullptr)
{
	float Buffer[256 * 2];
	float nPeak = 0;
	for (unsigned nFrames = SampleRate * nMs / 1000; nFrames;)
	{
		const size_t n = nFrames > 256 ? 256 : nFrames;
		const float nBlockPeak = OPL3MIDI::Render(p, Buffer, n, nGain);
		nPeak = nBlockPeak > nPeak ? nBlockPeak : nPeak;
		for (size_t i = 0; pInRange && i < n * 2; ++i)
			if (Buffer[i] > 1.0f || Buffer[i] < -1.0f)
				*pInRange = false;
		nFrames -= n;
	}
	return nPeak;
}

static uint32_t Msg(uint8_t a, uint8_t b = 0, uint8_t c = 0)
{
	return a | b << 8 | c << 16;
}

static int Checks()
{
	Check(adl_getBanksCount() == 79, "79 embedded banks");
	Check(std::strstr(adl_getBankNames()[72], "DMXOPL3") != nullptr, "bank 72 is DMXOPL3");
	Check(std::strstr(adl_getBankNames()[14], "laceholder") != nullptr, "grey-zone bank 14 left out");

	for (int nEmulator : {ADLMIDI_EMU_NUKED, ADLMIDI_EMU_DOSBOX})
	{
		ADL_MIDIPlayer* p = NewPlayer(72, nEmulator);
		const bool bNuked = nEmulator == ADLMIDI_EMU_NUKED;
		Check(std::strstr(adl_chipEmulatorName(p), bNuked ? "Nuked" : "DOSBox") != nullptr,
		      bNuked ? "Nuked emulator" : "DOSBox emulator");

		Check(Run(p, 100) < 1e-4f, "silent at start");
		OPL3MIDI::ShortMessage(p, Msg(0xC0, 0));
		OPL3MIDI::ShortMessage(p, Msg(0x90, 60, 100));
		const float nOn = Run(p, 300);
		Check(nOn > 0.05f, "Note On sounds");
		OPL3MIDI::ShortMessage(p, Msg(0x90, 60, 0));	// velocity 0 = Note Off
		Run(p, 1500);
		Check(Run(p, 100) < 1e-3f, "Note On with velocity 0 releases it");

		// Channel volume and the kernel's master volume (gain scaled), on a
		// sustained organ so the level holds still
		OPL3MIDI::ShortMessage(p, Msg(0xC0, 16));
		OPL3MIDI::ShortMessage(p, Msg(0x90, 60, 100));
		Run(p, 300);
		const float nFull = Run(p, 300);
		const float nHalf = Run(p, 300, Gain / 2);
		Check(nHalf < nFull * 0.6f && nHalf > nFull * 0.4f, "gain scales the output");
		OPL3MIDI::ShortMessage(p, Msg(0xB0, 7, 20));
		Run(p, 300);	// the level settles over the first window
		const float nLow = Run(p, 300);
		Check(nLow < nFull * 0.5f, "CC7 lowers the channel");
		OPL3MIDI::ShortMessage(p, Msg(0x80, 60, 0));
		Run(p, 1500);

		// System reset (0xFF) and adl_panic: everything stops
		OPL3MIDI::ShortMessage(p, Msg(0xB0, 7, 127));
		for (uint8_t n = 48; n < 72; n += 4)
			OPL3MIDI::ShortMessage(p, Msg(0x91, n, 120));
		Check(Run(p, 200) > 0.05f, "chord sounds");
		OPL3MIDI::ShortMessage(p, Msg(0xFF));
		Run(p, 1500);	// released notes fade as on the chip
		Check(Run(p, 100) < 1e-3f, "System Reset silences");

		// Loud: 12 notes at full velocity and a high gain stay within -1..1
		for (uint8_t c = 0; c < 6; ++c)
			for (uint8_t n = 40; n < 64; n += 12)
				OPL3MIDI::ShortMessage(p, Msg(0x90 | c, n, 127));
		bool bInRange = true;
		const float nLoud = Run(p, 300, 40.0f, &bInRange);
		Check(nLoud > 1.0f && bInRange, "clipped to -1..1");
		adl_panic(p);

		// GM System On SysEx is accepted; drums on channel 10
		const uint8_t GMOn[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
		Check(adl_rt_systemExclusive(p, GMOn, sizeof(GMOn)) == 1, "GM System On accepted");
		Run(p, 200);
		OPL3MIDI::ShortMessage(p, Msg(0x99, 38, 120));
		Check(Run(p, 200) > 0.02f, "snare on channel 10");

		// Pitch bend: data bytes LSB, MSB
		OPL3MIDI::ShortMessage(p, Msg(0xE2, 0x00, 0x60));
		OPL3MIDI::ShortMessage(p, Msg(0x92, 60, 100));
		Check(Run(p, 200) > 0.02f, "bent note sounds");

		// Bank change at run time (as Golem SET_PARAM 5)
		Check(adl_setBank(p, 0) == 0 && adl_setBank(p, 79) < 0, "banks 0..78 only");
		adl_close(p);
	}

	std::printf("%s\n", g_nFailures ? "FAILED" : "all passed");
	return g_nFailures ? 1 : 0;
}

static void PutLE(std::FILE* f, uint32_t v, int n)
{
	for (int i = 0; i < n; ++i)
		std::fputc(v >> (8 * i) & 0xFF, f);
}

static int RenderFile(const char* pEvents, const char* pOut, int nBank, int nEmulator)
{
	std::FILE* in = std::fopen(pEvents, "r");
	std::FILE* out = std::fopen(pOut, "wb");
	if (!in || !out)
		return 2;

	ADL_MIDIPlayer* p = NewPlayer(nBank, nEmulator);
	std::vector<int16_t> Samples;
	float Buffer[256 * 2];
	unsigned long nNow = 0;	// frames
	unsigned long nMs;
	unsigned b1, b2, b3;
	auto Advance = [&](unsigned long nTo) {
		while (nNow < nTo)
		{
			const size_t n = nTo - nNow > 256 ? 256 : nTo - nNow;
			OPL3MIDI::Render(p, Buffer, n, Gain);
			for (size_t i = 0; i < n * 2; ++i)
				Samples.push_back(static_cast<int16_t>(std::lrint(Buffer[i] * 32767.0f)));
			nNow += n;
		}
	};
	while (std::fscanf(in, "%lu %x %x %x", &nMs, &b1, &b2, &b3) == 4)
	{
		Advance(nMs * SampleRate / 1000);
		OPL3MIDI::ShortMessage(p, b1 | b2 << 8 | b3 << 16);
	}
	Advance(nNow + 2 * SampleRate);

	PutLE(out, 0x46464952, 4);
	PutLE(out, 36 + Samples.size() * 2, 4);
	PutLE(out, 0x45564157, 4);
	PutLE(out, 0x20746D66, 4);
	PutLE(out, 16, 4);
	PutLE(out, 1, 2);
	PutLE(out, 2, 2);
	PutLE(out, SampleRate, 4);
	PutLE(out, SampleRate * 4, 4);
	PutLE(out, 4, 2);
	PutLE(out, 16, 2);
	PutLE(out, 0x61746164, 4);
	PutLE(out, Samples.size() * 2, 4);
	for (int16_t s : Samples)
		PutLE(out, static_cast<uint16_t>(s), 2);
	std::fclose(out);
	std::fclose(in);
	adl_close(p);
	std::printf("%s: %.1f s, bank %d (%s)\n", pOut, Samples.size() / 2.0 / SampleRate, nBank, adl_getBankNames()[nBank]);
	return 0;
}

int main(int argc, char** argv)
{
	if (argc >= 3)
		return RenderFile(argv[1], argv[2], argc > 3 ? std::atoi(argv[3]) : 72,
		                  argc > 4 && !std::strcmp(argv[4], "dosbox") ? ADLMIDI_EMU_DOSBOX : ADLMIDI_EMU_NUKED);
	return Checks();
}
