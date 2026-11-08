// Host test for the FluidSynth part of the Golem sound-parameter protocol
// (golem-next docs/sound-params.md, section 9): area 10 section 02 byte by
// byte, the effects profile as a .cfg file both ways, and BLOB type 03
// through the protocol. The same code as golemd's (golem-linux), built as the
// kernel builds it (C++14).

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "golem/fxparams.h"
#include "golem/params.h"
#include "golem/wire.h"

using namespace golem;

static int g_nFailures = 0;
#define CHECK(x) Check((x), #x, __LINE__)

static void Check(bool bOk, const char* pName, int nLine)
{
	if (!bOk)
	{
		std::printf("FAIL line %d: %s\n", nLine, pName);
		++g_nFailures;
	}
}

static std::vector<uint8_t> ReadAll(const FXSettings& FX)
{
	std::vector<uint8_t> v;
	for (uint32_t p = 0; p <= fxparams::Last; ++p)
	{
		uint8_t b = 0xFF;
		CHECK(fxparams::Read(FX, p, b) == param::OK);
		v.push_back(b);
	}
	return v;
}

static void TestBytes()
{
	FXSettings FX;
	const auto Bytes = ReadAll(FX);
	// The defaults, as mt32-pi.cfg has them: room 0.2 = 200 = 48 01, width
	// 0.5 = 50, level 0.9 = 900 = 04 07, speed 0.3 = 300 = 2C 02, depth 8 =
	// 80, polyphony 200 = 48 01, gain 0.2 = 200 = 48 01
	const std::vector<uint8_t> Expected = {1, 0x48, 0x01, 0, 0, 50, 0, 0x04, 0x07, 1, 3,
	                                       0x50, 0x0F, 0x2C, 0x02, 80, 0, 0, 0x48, 0x01, 0x48, 0x01};
	CHECK(Bytes == Expected);
	uint8_t b;
	CHECK(fxparams::Read(FX, 0x16, b) == param::BadAddress);

	// Everything written at once and read back
	FXSettings New;
	const std::vector<uint8_t> All = {0, 0x7F, 0x03, 0x10, 0x00, 0x10, 0x4E, 0x00, 0x00, 1, 99,
	                                  0x10, 0x4E, 0x64, 0x00, 0x00, 0x14, 1, 0x7F, 0x7F, 0x10, 0x4E};
	size_t nBad = 99;
	CHECK(fxparams::Write(New, 0, All.data(), All.size(), nBad) == param::OK);
	CHECK(ReadAll(New) == All);
	CHECK(!New.Reverb && New.RoomSize == 0x1FF && New.Width == 10000 && New.Voices == 99 &&
	      New.ChorusLevel == 10000 && New.Speed == 100 && New.Depth == 2560 && New.ChorusType == 1 &&
	      New.Polyphony == 16383 && New.Gain == 10000);

	// One half of a 14-bit value keeps the other: level to 0x0400 + 0x10
	FXSettings Half;
	const uint8_t Low = 0x10;
	CHECK(fxparams::Write(Half, 0x07, &Low, 1, nBad) == param::OK && Half.Level == (900 & ~0x7F) + 0x10);

	// Out of range: nothing changes, the position is the byte that completes it
	FXSettings Keep, Before;
	const uint8_t RoomTooBig[] = {0x00, 0x10}; // 2048
	CHECK(fxparams::Write(Keep, 0x01, RoomTooBig, 2, nBad) == param::BadValue && nBad == 1 && Keep == Before);
	const uint8_t Speed0[] = {1, 0, 0}; // chorus on, speed 0: under 100
	CHECK(fxparams::Write(Keep, 0x0C, Speed0, 3, nBad) == param::BadValue && nBad == 2 && Keep == Before);
	const uint8_t Voices[] = {100};
	CHECK(fxparams::Write(Keep, 0x0A, Voices, 1, nBad) == param::BadValue && nBad == 0);
	const uint8_t Switch[] = {2};
	CHECK(fxparams::Write(Keep, 0x00, Switch, 1, nBad) == param::BadValue && nBad == 0);
	const uint8_t Poly0[] = {0, 0};
	CHECK(fxparams::Write(Keep, 0x12, Poly0, 2, nBad) == param::BadValue && nBad == 1);
	const uint8_t Past[] = {1, 1, 1};
	CHECK(fxparams::Write(Keep, 0x14, Past, 3, nBad) == param::BadAddress && nBad == 2 && Keep == Before);
}

static void TestConfig()
{
	// mt32-pi's own profile for GeneralUser GS (sdcard/soundfonts)
	const char Profile[] = "# comment\n"
	                       "gain = 0.2\n\n"
	                       "reverb = on\n"
	                       "reverb_damping = 0.4\n"
	                       "reverb_level = 0.4\n"
	                       "reverb_room_size = 0.4\n"
	                       "reverb_width = 0.75\n\n"
	                       "chorus = on\n"
	                       "chorus_depth = 4.5\n"
	                       "chorus_level = 0.5\n"
	                       "chorus_voices = 3\n"
	                       "chorus_speed = 0.35\n";
	FXSettings FX;
	size_t nLine = 99;
	CHECK(fxparams::FromConfig(Profile, sizeof(Profile) - 1, FX, nLine) && nLine == 0);
	CHECK(FX.Damping == 400 && FX.Level == 400 && FX.RoomSize == 400 && FX.Width == 75 && FX.Depth == 45 &&
	      FX.ChorusLevel == 500 && FX.Speed == 350 && FX.Voices == 3 && FX.Gain == 200);
	// Left out: as before
	CHECK(FX.Polyphony == 200 && FX.ChorusType == 0);

	// Out and back in, for many settings
	std::mt19937 Random(7);
	bool bAllBack = true;
	for (int n = 0; n < 500; ++n)
	{
		FXSettings a;
		a.Reverb      = Random() & 1;
		a.RoomSize    = Random() % 1001;
		a.Damping     = Random() % 1001;
		a.Width       = Random() % 10001;
		a.Level       = Random() % 1001;
		a.Chorus      = Random() & 1;
		a.Voices      = Random() % 100;
		a.ChorusLevel = Random() % 10001;
		a.Speed       = 100 + Random() % 4901;
		a.Depth       = Random() % 2561;
		a.ChorusType  = Random() & 1;
		a.Polyphony   = 1 + Random() % 16383;
		a.Gain        = Random() % 10001;
		const std::string Text = fxparams::ToConfig(a);
		FXSettings b;
		b.Polyphony = 1;
		bAllBack &= fxparams::FromConfig(Text.data(), Text.size(), b, nLine) && b == a;
	}
	CHECK(bAllBack);

	// Faults: the line, and nothing changed
	const FXSettings Before = FX;
	const char Unknown[] = "reverb = on\nreverb_colour = 3\n";
	CHECK(!fxparams::FromConfig(Unknown, sizeof(Unknown) - 1, FX, nLine) && nLine == 2 && FX == Before);
	const char Range[] = "\n\nchorus_speed = 9\n";
	CHECK(!fxparams::FromConfig(Range, sizeof(Range) - 1, FX, nLine) && nLine == 3 && FX == Before);
	const char Word[] = "chorus = maybe\n";
	CHECK(!fxparams::FromConfig(Word, sizeof(Word) - 1, FX, nLine) && nLine == 1);
	const char NoEquals[] = "reverb on\n";
	CHECK(!fxparams::FromConfig(NoEquals, sizeof(NoEquals) - 1, FX, nLine) && nLine == 1);
	// Sections, CRLF, ; comments and capitals are fine
	const char Windows[] = "[fluidsynth]\r\nChorus_Type = Triangle ; the other one\r\npolyphony=64\r\n";
	CHECK(fxparams::FromConfig(Windows, sizeof(Windows) - 1, FX, nLine) && FX.ChorusType == 1 && FX.Polyphony == 64);
}

class CHost : public ParamHost
{
public:
	ParamInfo Info() override { return ParamInfo(); }
	uint8_t AreaRead(uint8_t, uint32_t, size_t, std::vector<uint8_t>&) override { return param::BadAddress; }
	uint8_t AreaWrite(uint8_t, uint32_t, const uint8_t*, size_t, size_t&) override { return param::BadAddress; }
	uint8_t BlobCommit(uint8_t nType, uint8_t nTarget, std::vector<uint8_t>&& file) override
	{
		size_t nLine = 0;
		if (nType != param::BlobFX || nTarget != 0)
			return param::Unavailable;
		return fxparams::FromConfig(reinterpret_cast<const char*>(file.data()), file.size(), FX, nLine) ?
		           static_cast<uint8_t>(param::OK) :
		           static_cast<uint8_t>(param::BadValue);
	}
	uint8_t BlobGet(uint8_t nType, uint8_t nOrigin, std::vector<uint8_t>& file) override
	{
		if (nType != param::BlobFX || nOrigin != 0)
			return param::Unavailable;
		const std::string t = fxparams::ToConfig(FX);
		file.assign(t.begin(), t.end());
		return param::OK;
	}
	FXSettings FX;
};

static std::vector<uint8_t> Request(uint8_t tt, uint8_t cc, const std::vector<uint8_t>& body, uint8_t nFlags = 0)
{
	std::vector<uint8_t> f = {0xF0, 0x7D, 0x47, 0x4C, 0x4D, 0x01, tt, cc, nFlags};
	f.insert(f.end(), body.begin(), body.end());
	f.push_back(wire::Checksum(f.data() + 8, f.size() - 8));
	f.push_back(0xF7);
	return f;
}

static void TestProtocol()
{
	CHost Host;
	Host.FX.Speed = 1234;
	std::vector<std::vector<uint8_t>> Out;
	ParamProtocol Protocol(Host, [&Out](const uint8_t* p, size_t n) { Out.emplace_back(p, p + n); });
	auto Send = [&](const std::vector<uint8_t>& f) {
		Out.clear();
		Protocol.Handle(f.data(), f.size());
		return Out;
	};

	// Download: about 330 bytes, so two chunks
	std::vector<uint8_t> Text;
	std::vector<std::vector<uint8_t>> r;
	for (uint8_t tt = 0x01; tt < 0x05; ++tt)
	{
		std::vector<uint8_t> b = {0x04, param::BlobFX, 0};
		wire::Put21(b, static_cast<uint32_t>(Text.size()));
		wire::Put14(b, 256);
		r = Send(Request(tt, param::Blob, b));
		if (r.size() != 1 || r[0][7] != param::Data)
			break; // past the end: ERROR 08
		std::vector<uint8_t> Part;
		CHECK(wire::Unpack(Part, r[0].data() + 13, r[0].size() - 15));
		Text.insert(Text.end(), Part.begin(), Part.end());
	}
	CHECK(r.size() == 1 && r[0][7] == param::Error && r[0][9] == param::BadAddress);
	const std::string Got(Text.begin(), Text.end());
	CHECK(Got == fxparams::ToConfig(Host.FX) && Got.find("chorus_speed = 1.234\n") != std::string::npos);

	// Upload a changed one
	std::string Changed = "reverb = off\nchorus_type = triangle\n";
	std::vector<uint8_t> Begin = {0x00, param::BlobFX, 0};
	wire::Put21(Begin, static_cast<uint32_t>(Changed.size()));
	r = Send(Request(0x12, param::Blob, Begin));
	CHECK(r.size() == 1 && r[0][7] == param::Ready);
	std::vector<uint8_t> Chunk = {0x01, 0, 0};
	wire::Pack(Chunk, reinterpret_cast<const uint8_t*>(Changed.data()), Changed.size());
	Send(Request(0x03, param::Blob, Chunk));
	r = Send(Request(0x04, param::Blob, {0x02}));
	CHECK(r.size() == 2 && r[1][7] == param::Ready);
	CHECK(!Host.FX.Reverb && Host.FX.ChorusType == 1 && Host.FX.Speed == 1234);

	// A bad profile: ERROR 04 at END, nothing changed
	const FXSettings Before = Host.FX;
	std::string Bad = "chorus_voices = 200\n";
	Begin = {0x00, param::BlobFX, 0};
	wire::Put21(Begin, static_cast<uint32_t>(Bad.size()));
	Send(Request(0x05, param::Blob, Begin));
	Chunk = {0x01, 0, 0};
	wire::Pack(Chunk, reinterpret_cast<const uint8_t*>(Bad.data()), Bad.size());
	Send(Request(0x06, param::Blob, Chunk));
	r = Send(Request(0x07, param::Blob, {0x02}));
	CHECK(r.size() == 2 && r[1][7] == param::Error && r[1][9] == param::BadValue && Host.FX == Before);
}

int main()
{
	TestBytes();
	TestConfig();
	TestProtocol();
	std::printf("%s\n", g_nFailures ? "FAILED" : "all checks passed");
	return g_nFailures ? 1 : 0;
}
