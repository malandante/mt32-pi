// Host test for include/synth/opl3midi.h with libADLMIDI built as the kernel
// builds it (Makefile target adlmidi, without the cross toolchain).
//
//   test_opl3midi                 the checks
//   test_opl3midi EVENTS OUT.WAV [BANK|FILE.wopl [EMULATOR]]
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

// A WOPL file from libADLMIDI's sources (a free bank), as a user would put
// it in SD:opl3
static const char BankFile[] = "external/libadlmidi/fm_banks/wopl_files/GM-By-J.A.Nguyen-and-Wohlstand.wopl";

static std::vector<uint8_t> ReadFile(const char* pPath)
{
	std::vector<uint8_t> Data;
	if (std::FILE* f = std::fopen(pPath, "rb"))
	{
		for (int c; (c = std::fgetc(f)) != EOF;)
			Data.push_back(static_cast<uint8_t>(c));
		std::fclose(f);
	}
	return Data;
}

// As COPL3Synth::CreatePlayer: an embedded bank number, or a .wopl path
static ADL_MIDIPlayer* NewPlayer(const char* pBank, int nEmulator = ADLMIDI_EMU_NUKED)
{
	if (std::strstr(pBank, ".wopl"))
	{
		const std::vector<uint8_t> Data = ReadFile(pBank);
		return Data.empty() ? nullptr : OPL3MIDI::NewPlayer(SampleRate, nEmulator, 1, 0, Data.data(), Data.size());
	}
	return OPL3MIDI::NewPlayer(SampleRate, nEmulator, 1, std::atoi(pBank));
}

static ADL_MIDIPlayer* NewPlayer(int nBank, int nEmulator = ADLMIDI_EMU_NUKED)
{
	return OPL3MIDI::NewPlayer(SampleRate, nEmulator, 1, nBank);
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

		adl_close(p);
	}

	// Banks: embedded 0-78; past that, players from WOPL files
	Check(NewPlayer(78) != nullptr && NewPlayer(79) == nullptr, "embedded banks 0..78 only");
	{
		ADL_MIDIPlayer* p = NewPlayer(BankFile);
		Check(p != nullptr, "WOPL bank file loads");
		if (p)
		{
			OPL3MIDI::ShortMessage(p, Msg(0xC0, 48));
			OPL3MIDI::ShortMessage(p, Msg(0x90, 60, 100));
			Check(Run(p, 300) > 0.05f, "and plays");
			adl_close(p);
		}
		std::vector<uint8_t> Data = ReadFile(BankFile);
		const uint8_t Junk[64] = {'n', 'o', 't', ' ', 'a', ' ', 'b', 'a', 'n', 'k'};
		Check(OPL3MIDI::NewPlayer(SampleRate, ADLMIDI_EMU_NUKED, 1, 0, Junk, sizeof(Junk)) == nullptr,
		      "a file that is not WOPL is refused");
		Check(Data.size() > 100 &&
		      OPL3MIDI::NewPlayer(SampleRate, ADLMIDI_EMU_NUKED, 1, 0, Data.data(), Data.size() / 2) == nullptr,
		      "a cut WOPL file is refused");
	}

	// A slow release (bank 58, program 107) is cut by the stop sweep of All
	// Sound Off on all 16 channels, as the kernel does it (adl_reset)
	{
		ADL_MIDIPlayer* p = NewPlayer(58);
		OPL3MIDI::TSoundOffSweep Sweep;
		auto Send = [&](uint32_t m) {
			OPL3MIDI::ShortMessage(p, m);
			if (Sweep.Message(m))
				adl_reset(p);
		};
		Send(Msg(0xC0, 107));
		Send(Msg(0x90, 60, 100));
		Run(p, 500);
		Send(Msg(0x80, 60, 0));
		Run(p, 1000);
		const float nRinging = Run(p, 200);
		for (uint8_t c = 0; c < 16; ++c)
		{
			Send(Msg(0xB0 | c, 64, 0));
			Send(Msg(0xB0 | c, 123, 0));
			if (c < 15)
				Send(Msg(0xB0 | c, 120, 0));
		}
		const float nPartSweep = Run(p, 200);
		Send(Msg(0xBF, 120, 0));
		Run(p, 50);
		Check(nRinging > 1e-3f && nPartSweep > 1e-4f && Run(p, 200) < 1e-5f,
		      "the stop sweep silences a long release");
		Send(Msg(0xC0, 0));
		Send(Msg(0x90, 60, 100));
		Check(Run(p, 300) > 0.02f, "and the player plays again");
		adl_close(p);
	}

	// A bank change builds a new player, so the sound does not depend on the
	// banks used before: two new players for bank 72 play the same samples
	{
		std::vector<float> Out[2];
		for (auto& o : Out)
		{
			ADL_MIDIPlayer* p = NewPlayer(72);
			OPL3MIDI::ShortMessage(p, Msg(0xC0, 19));
			for (uint8_t n : {48, 55, 60, 64})
				OPL3MIDI::ShortMessage(p, Msg(0x90, n, 100));
			o.resize(48000 * 2);
			for (size_t i = 0; i < 48000; i += 256)
				OPL3MIDI::Render(p, o.data() + i * 2, 48000 - i < 256 ? 48000 - i : 256, Gain);
			adl_close(p);
		}
		Check(Out[0] == Out[1], "same bank, same sound");
	}

	std::printf("%s\n", g_nFailures ? "FAILED" : "all passed");
	return g_nFailures ? 1 : 0;
}

static void PutLE(std::FILE* f, uint32_t v, int n)
{
	for (int i = 0; i < n; ++i)
		std::fputc(v >> (8 * i) & 0xFF, f);
}

static int RenderFile(const char* pEvents, const char* pOut, const char* pBank, int nEmulator)
{
	std::FILE* in = std::fopen(pEvents, "r");
	std::FILE* out = std::fopen(pOut, "wb");
	if (!in || !out)
		return 2;

	ADL_MIDIPlayer* p = NewPlayer(pBank, nEmulator);
	if (!p)
		return 2;
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
	std::printf("%s: %.1f s, bank %s\n", pOut, Samples.size() / 2.0 / SampleRate, pBank);
	return 0;
}

int main(int argc, char** argv)
{
	if (argc >= 3)
		return RenderFile(argv[1], argv[2], argc > 3 ? argv[3] : "72",
		                  argc > 4 && !std::strcmp(argv[4], "dosbox") ? ADLMIDI_EMU_DOSBOX : ADLMIDI_EMU_NUKED);
	return Checks();
}
