// Host test for the Golem sound-parameter protocol (golem-next
// docs/sound-params.md), OPL3 part: the wire encodings, the WOPL/WOPLX bank
// model, the OPL3 working bank with real libADLMIDI players, and the protocol
// framing against a fake host. The same code as golemd's (golem-linux), built
// as the kernel builds it (C++14), with libADLMIDI from the CI host build.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <adlmidi.h>

#include "golem/opl3work.h"
#include "golem/oplbank.h"
#include "golem/params.h"
#include "golem/wire.h"
#include "synth/opl3midi.h"

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

constexpr unsigned SampleRate = 48000;

static ADL_MIDIPlayer* Embedded(int nBank)
{
	return OPL3MIDI::NewPlayer(SampleRate, ADLMIDI_EMU_NUKED, 1, nBank);
}

static ADL_MIDIPlayer* FromBytes(const uint8_t* pData, size_t nSize)
{
	return OPL3MIDI::NewPlayer(SampleRate, ADLMIDI_EMU_NUKED, 1, 0, pData, nSize);
}

// Peak of a note on channel 1, program 0, rendered for 0.2 s
static float NotePeak(ADL_MIDIPlayer* p)
{
	OPL3MIDI::ShortMessage(p, 0x0000C0);
	OPL3MIDI::ShortMessage(p, 0x7F3C90);
	std::vector<float> Buf(9600 * 2);
	const float nPeak = OPL3MIDI::Render(p, Buf.data(), 9600, 1.0f);
	OPL3MIDI::ShortMessage(p, 0x003C80);
	adl_panic(p);
	// Let the release ring out, so the next note is measured alone
	for (int i = 0; i < 20; ++i)
		OPL3MIDI::Render(p, Buf.data(), 9600, 1.0f);
	return nPeak;
}

static void TestWire()
{
	const uint8_t m[] = {0x10, 0x00, 0x00, 0x00, 0x64};
	std::vector<uint8_t> v(m, m + sizeof(m));
	v.push_back(wire::Checksum(m, sizeof(m)));
	CHECK(wire::ChecksumOK(v.data(), v.size()));
	v[2] ^= 1;
	CHECK(!wire::ChecksumOK(v.data(), v.size()));
	// Roland's own example: 41 10 16 12 | 10 00 04 64 | checksum 08
	const uint8_t r[] = {0x10, 0x00, 0x04, 0x64};
	CHECK(wire::Checksum(r, sizeof(r)) == 0x08);

	std::mt19937 rng(1);
	for (size_t n : {0u, 1u, 6u, 7u, 8u, 13u, 14u, 15u, 256u, 1000u})
	{
		std::vector<uint8_t> in(n);
		for (auto& b : in)
			b = static_cast<uint8_t>(rng());
		std::vector<uint8_t> packed, out;
		wire::Pack(packed, in.data(), in.size());
		CHECK(packed.size() == wire::PackedSize(n));
		bool b7 = true;
		for (const uint8_t b : packed)
			b7 &= b < 0x80;
		CHECK(b7);
		CHECK(wire::Unpack(out, packed.data(), packed.size()) && out == in);
	}
	std::vector<uint8_t> out;
	const uint8_t bad[] = {0x7F, 0x01}; // header announces 7 bytes, one there
	CHECK(!wire::Unpack(out, bad, sizeof(bad)));
}

static void TestBank()
{
	ADL_MIDIPlayer* pPlayer = Embedded(72);
	CHECK(pPlayer != nullptr);
	OPLBank Bank;
	CHECK(Bank.FromPlayer(pPlayer));
	adl_close(pPlayer);
	CHECK(Bank.MelodicBanks() >= 1 && Bank.PercussionBanks() >= 1);
	const std::vector<uint8_t> File = Bank.Save();
	CHECK(File.size() > 19 && memcmp(File.data(), "WOPL3-BANK", 11) == 0);
	{
		OPLBank Again;
		CHECK(Again.Load(File.data(), File.size()));
		CHECK(Again.Save() == File);
	}

	uint8_t rec[128], back[128];
	Bank.ReadRecord(0, 0, rec);
	rec[0x20 + 6] = 63;            // operator 1 total level: quietest
	memcpy(rec + 0x60, "Test piano", 10);
	rec[0x11] = 64 + 12;           // fine tune +12 cents (WOPLX)
	rec[0x13] = 3;                 // velocity curve 3 (WOPLX)
	size_t nBad = 99;
	CHECK(Bank.WriteRecord(0, 0, rec, nBad) == OPLBank::OK);
	Bank.ReadRecord(0, 0, back);
	CHECK(memcmp(rec, back, 128) == 0);

	uint8_t w[128];
	memcpy(w, rec, 128);
	w[0x27] = 16; // attack 0-15
	CHECK(Bank.WriteRecord(0, 0, w, nBad) == OPLBank::BadValue && nBad == 0x27);
	Bank.ReadRecord(0, 0, back);
	CHECK(memcmp(rec, back, 128) == 0); // nothing changed
	CHECK(!Bank.HasInstrument(63, 0) && Bank.WriteRecord(63, 0, rec, nBad) == OPLBank::BadAddress);

	// WOPLX: the extras survive a save and a load; libADLMIDI reads it as WOPL
	const std::vector<uint8_t> X = Bank.Save();
	CHECK(X.size() > File.size());
	OPLBank Y;
	CHECK(Y.Load(X.data(), X.size()));
	CHECK(Y.GetExtras(0, 0).FineTune1 == 12 && Y.GetExtras(0, 0).Curve == 3);
	ADL_MIDIPlayer* p2 = FromBytes(X.data(), X.size());
	CHECK(p2 != nullptr);
	if (p2)
		adl_close(p2);
	std::vector<uint8_t> Z = X;
	Z.back() ^= 0x55; // a damaged block is ignored; the bank still loads
	OPLBank W;
	CHECK(W.Load(Z.data(), Z.size()) && W.GetExtras(0, 0).Empty());

	// Directory and header
	uint8_t v = 9;
	CHECK(Bank.ReadDirectory(0x000000, v) == OPLBank::OK && v == 1);   // melodic bank 0 present
	CHECK(Bank.ReadDirectory(0x40 << 7, v) == OPLBank::OK && v == 1);  // percussion bank 0
	CHECK(Bank.ReadDirectory(0x3F << 7, v) == OPLBank::OK && v == 0);  // melodic bank 63: none
	CHECK(Bank.WriteDirectory(0x00, 0) == OPLBank::BadAddress);        // presence: read-only
	CHECK(Bank.WriteDirectory((1u << 14) | 0x02, 10) == OPLBank::BadValue); // volume model 0-9
}

static void TestWorkingBank()
{
	ADL_MIDIPlayer* pChosen = Embedded(72);
	const float nBefore = NotePeak(pChosen);
	CHECK(nBefore > 0.01f);

	OPL3Work Work;
	auto ReadNone = [](std::vector<uint8_t>&) { return false; };
	auto PrepareChosen = [] { return Embedded(72); };
	CHECK(!Work.Made() && !Work.Changed());
	const OPLBank* pBank = Work.Get(pChosen, true, ReadNone);
	CHECK(pBank != nullptr && Work.Made() && !Work.Changed());

	// Program 0 silent: every operator at the quietest level, heard live
	uint8_t r[128];
	pBank->ReadRecord(0, 0, r);
	for (int op = 0; op < 4; ++op)
		r[0x20 + op * 0x10 + 6] = 63;
	size_t nBad = 0;
	std::vector<int> Live;
	CHECK(Work.WriteRecords(0, 1, r, nBad, Live) == OPLBank::OK && Live.size() == 1 && Live[0] == 0);
	for (const int nKey : Live)
		Work.Apply(pChosen, nKey);
	CHECK(NotePeak(pChosen) < nBefore * 0.1f);
	CHECK(Work.Changed());

	// GM System On and the stop sweep build a new player: the edit stays
	ADL_MIDIPlayer* pRebuilt = Work.NewPlayer(PrepareChosen, FromBytes);
	CHECK(pRebuilt != nullptr && NotePeak(pRebuilt) < nBefore * 0.1f);
	adl_close(pRebuilt);

	// A refused byte changes nothing
	r[0x27] = 16;
	CHECK(Work.WriteRecords(0, 1, r, nBad, Live) == OPLBank::BadValue && nBad == 0x27);
	r[0x27] = 0;

	// A new header: from bytes from now on, and a new player is needed
	bool bNewPlayer = false;
	const uint8_t Deep = 1;
	CHECK(Work.WriteDirectory((1u << 14) | 0x01, &Deep, 1, nBad, bNewPlayer) == OPLBank::OK && bNewPlayer);
	pRebuilt = Work.NewPlayer(PrepareChosen, FromBytes);
	CHECK(pRebuilt != nullptr && NotePeak(pRebuilt) < nBefore * 0.1f);
	adl_close(pRebuilt);

	// Choosing a bank replaces it
	Work.Reset();
	CHECK(!Work.Made() && !Work.Changed());
	pRebuilt = Work.NewPlayer(PrepareChosen, FromBytes);
	CHECK(pRebuilt != nullptr && NotePeak(pRebuilt) > nBefore * 0.5f);
	adl_close(pRebuilt);

	// An uploaded bank becomes the working bank and survives a rebuild
	OPLBank Up;
	CHECK(Up.FromPlayer(pChosen)); // the live player, program 0 still silent
	ADL_MIDIPlayer* pUp = Work.PrepareFrom(Up, FromBytes);
	CHECK(pUp != nullptr);
	Work.SetUploaded(std::move(Up));
	CHECK(Work.Uploaded() && Work.Changed());
	CHECK(pUp && NotePeak(pUp) < nBefore * 0.1f);
	if (pUp)
		adl_close(pUp);
	pRebuilt = Work.NewPlayer(PrepareChosen, FromBytes);
	CHECK(pRebuilt != nullptr && NotePeak(pRebuilt) < nBefore * 0.1f);
	adl_close(pRebuilt);

	// A file bank: read through the callback
	OPL3Work FileWork;
	const std::vector<uint8_t> Bytes = Work.Bank().Save();
	auto ReadFile = [&Bytes](std::vector<uint8_t>& d) { d = Bytes; return true; };
	CHECK(FileWork.Get(nullptr, false, ReadFile) != nullptr && !FileWork.Changed());
	CHECK(FileWork.WriteRecords(0, 1, r, nBad, Live) == OPLBank::OK && FileWork.Changed());

	adl_close(pChosen);
}

// A fake engine: area 10 is 128 plain bytes, BLOB keeps the file
class CFakeHost : public ParamHost
{
public:
	ParamInfo Info() override
	{
		ParamInfo i;
		i.Platform = 0;
		i.Version[0] = 1;
		i.Version[1] = 3;
		i.Engines = 4;
		i.Functions = param::FnAreas | param::FnOPL3Instr | param::FnWOPLUp | param::FnWOPLDown;
		i.Name = "mt32-pi golem-1.3.0";
		return i;
	}
	uint8_t AreaRead(uint8_t nArea, uint32_t nAddress, size_t nSize, std::vector<uint8_t>& out) override
	{
		if (nArea != param::Settings || nAddress + nSize > 128)
			return param::BadAddress;
		out.assign(Settings + nAddress, Settings + nAddress + nSize);
		return param::OK;
	}
	uint8_t AreaWrite(uint8_t nArea, uint32_t nAddress, const uint8_t* p, size_t n, size_t& nBad) override
	{
		if (nArea != param::Settings || nAddress + n > 128)
		{
			nBad = 0;
			return param::BadAddress;
		}
		memcpy(Settings + nAddress, p, n);
		return param::OK;
	}
	uint8_t BlobCommit(uint8_t nType, uint8_t, std::vector<uint8_t>&& f) override
	{
		if (nType != param::BlobWOPL)
			return param::Unavailable;
		File = std::move(f);
		return param::OK;
	}
	uint8_t BlobGet(uint8_t nType, uint8_t, std::vector<uint8_t>& f) override
	{
		if (nType != param::BlobWOPL)
			return param::Unavailable;
		f = File;
		return param::OK;
	}

	uint8_t Settings[128] = {};
	std::vector<uint8_t> File;
};

struct TFrames
{
	std::vector<std::vector<uint8_t>> List;
	void Add(const uint8_t* p, size_t n) { List.emplace_back(p, p + n); }
};

static std::vector<uint8_t> Request(uint8_t tt, uint8_t cc, std::vector<uint8_t> body, uint8_t nFlags = 0)
{
	std::vector<uint8_t> Payload = {nFlags};
	Payload.insert(Payload.end(), body.begin(), body.end());
	Payload.push_back(wire::Checksum(Payload.data(), Payload.size()));
	std::vector<uint8_t> Frame = {0xF0, 0x7D, 0x47, 0x4C, 0x4D, 0x01, tt, cc};
	Frame.insert(Frame.end(), Payload.begin(), Payload.end());
	Frame.push_back(0xF7);
	return Frame;
}

// Payload of a reply: after F0 7D 47 4C 4D 01 tt rr cc, without F7
static std::vector<uint8_t> Body(const std::vector<uint8_t>& f)
{
	return f.size() > 10 ? std::vector<uint8_t>(f.begin() + 9, f.end() - 1) : std::vector<uint8_t>();
}

static void TestProtocol()
{
	CFakeHost Host;
	TFrames Out;
	ParamProtocol Protocol(Host, [&Out](const uint8_t* p, size_t n) { Out.Add(p, n); });
	auto Send = [&](const std::vector<uint8_t>& f) {
		Out.List.clear();
		Protocol.Handle(f.data(), f.size());
		return Out.List;
	};

	// GET_INFO page 0: INFO, revision 1.1, platform 0, checksum
	auto r = Send(Request(0x01, param::GetInfo, {0}));
	CHECK(r.size() == 1 && r[0][7] == param::Info);
	if (r.size() == 1)
	{
		const auto b = Body(r[0]);
		CHECK(b.size() > 4 && b[0] == 0 && b[1] == 1 && b[2] == 1 && b[3] == 0);
		CHECK(!b.empty() && wire::Checksum(b.data(), b.size() - 1) == b.back());
	}
	r = Send(Request(0x02, param::GetInfo, {1}));
	{
		const auto b = r.size() == 1 ? Body(r[0]) : std::vector<uint8_t>();
		CHECK(b.size() > 2 && std::string(b.begin() + 1, b.end() - 1).find("mt32-pi golem-1.3.0") == 0);
	}

	// A wrong checksum: ERROR 07
	auto Bad = Request(0x03, param::GetInfo, {0});
	Bad[Bad.size() - 2] ^= 1;
	r = Send(Bad);
	CHECK(r.size() == 1 && r[0][7] == param::Error && Body(r[0])[0] == param::BadChecksum);

	// PARAM_WRITE confirmed: READY; unconfirmed: silent; PARAM_READ: DATA
	r = Send(Request(0x04, param::Write, {param::Settings, 0, 0, 5, 55, 56}, 1));
	CHECK(r.size() == 1 && r[0][7] == param::Ready);
	r = Send(Request(0x05, param::Write, {param::Settings, 0, 0, 7, 57}));
	CHECK(r.empty() && Host.Settings[7] == 57);
	r = Send(Request(0x06, param::Read, {param::Settings, 0, 0, 5, 3, 0}));
	CHECK(r.size() == 1 && r[0][7] == param::Data);
	if (r.size() == 1)
	{
		const auto b = Body(r[0]);
		CHECK(b.size() == 4 + 3 + 1 && b[4] == 55 && b[5] == 56 && b[6] == 57);
	}
	// Outside the area: ERROR 08
	r = Send(Request(0x07, param::Read, {param::Settings, 0, 1, 0, 1, 0}));
	CHECK(r.size() == 1 && Body(r[0])[0] == param::BadAddress);

	// BLOB upload of 600 bytes in 256-byte chunks, then read it back
	std::vector<uint8_t> File(600);
	for (size_t i = 0; i < File.size(); ++i)
		File[i] = static_cast<uint8_t>(i * 7);
	r = Send(Request(0x10, param::Blob, {0x00, param::BlobWOPL, 0x00, 0, (600 >> 7) & 0x7F, 600 & 0x7F}));
	CHECK(r.size() == 1 && r[0][7] == param::Ready);
	uint8_t tt = 0x11;
	for (size_t i = 0, nSeq = 0; i < File.size(); i += 256, ++nSeq)
	{
		std::vector<uint8_t> Chunk(File.begin() + i, File.begin() + std::min(File.size(), i + 256));
		std::vector<uint8_t> b = {0x01, static_cast<uint8_t>(nSeq & 0x7F), static_cast<uint8_t>(nSeq >> 7)};
		wire::Pack(b, Chunk.data(), Chunk.size());
		r = Send(Request(tt++, param::Blob, b));
		CHECK(r.empty());
	}
	r = Send(Request(tt++, param::Blob, {0x02}));
	CHECK(r.size() == 2 && r[0][7] == param::Accepted && r[1][7] == param::Ready);
	CHECK(Host.File == File);

	r = Send(Request(tt++, param::Blob, {0x04, param::BlobWOPL, 0x00, 0, 0, 0, 0, 0}));
	CHECK(r.size() == 1 && r[0][7] == param::Data);
	std::vector<uint8_t> Back;
	while (Back.size() < File.size() && tt < 0x40)
	{
		const size_t nAt = Back.size();
		r = Send(Request(tt++, param::Blob,
		                 {0x04, param::BlobWOPL, 0x00, static_cast<uint8_t>((nAt >> 14) & 0x7F),
		                  static_cast<uint8_t>((nAt >> 7) & 0x7F), static_cast<uint8_t>(nAt & 0x7F), 0, 2}));
		if (r.size() != 1 || r[0][7] != param::Data)
		{
			CHECK(false);
			break;
		}
		const auto b = Body(r[0]);
		std::vector<uint8_t> Part;
		CHECK(wire::Unpack(Part, b.data() + 4, b.size() - 5));
		Back.insert(Back.end(), Part.begin(), Part.end());
	}
	CHECK(Back == File);

	// A chunk out of order: ERROR 0B
	Send(Request(0x50, param::Blob, {0x00, param::BlobWOPL, 0x00, 0, 0, 10}));
	std::vector<uint8_t> b = {0x01, 5, 0};
	const uint8_t Two[] = {1, 2};
	wire::Pack(b, Two, 2);
	r = Send(Request(0x51, param::Blob, b));
	CHECK(r.size() == 1 && Body(r[0])[0] == param::Sequence);

	// OPL3_REG is not there yet: unknown command
	r = Send(Request(0x52, param::OPL3Reg, {0}));
	CHECK(r.size() == 1 && r[0][7] == param::Error && Body(r[0])[0] == param::UnknownCommand);
}

int main()
{
	TestWire();
	TestBank();
	TestWorkingBank();
	TestProtocol();
	std::printf("%s\n", g_nFailures ? "FAILED" : "all checks passed");
	return g_nFailures ? 1 : 0;
}
