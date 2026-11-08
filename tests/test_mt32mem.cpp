// Host test for the MT-32 part of the Golem sound-parameter protocol (golem-next
// docs/sound-params.md, section 5): areas 00 and 04, native RQ1, the dump
// file, and BLOB type 04 through the protocol. Munt needs the user's ROMs, so
// it is replaced by a model of its memory with Munt 2.7.0's rules
// (MemoryRegion.h, Synth::writeSysexGlobal, Synth::readMemory): zones that
// clamp a read at their end, a DT1 that runs on into the next zone, and the
// timbre memory written 128 timbres further on than it is read. The same code
// as golemd's (golem-linux), built as the kernel builds it (C++14).

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "golem/mt32mem.h"
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

using mt32mem::Address;

// Munt's memory, by its own rules
class CMuntModel : public MT32Access
{
public:
	struct Region
	{
		uint32_t Start, EntrySize, Entries;
		bool Readable;
		bool Timbres;
	};

	CMuntModel() : Memory(1 << 21, 0)
	{
		// Something recognisable everywhere: the ROM timbres and the rest
		for (size_t i = 0; i < Memory.size(); ++i)
			Memory[i] = static_cast<uint8_t>((i * 31 + (i >> 8)) & 0x7F);
	}

	static const Region* Find(uint32_t a)
	{
		static const Region Regions[] = {
			{Address(0x03, 0x00, 0x00), 16, 9, true, false},
			{Address(0x03, 0x01, 0x10), 4, 85, true, false},
			{Address(0x04, 0x00, 0x00), 246, 8, true, false},
			{Address(0x05, 0x00, 0x00), 8, 128, true, false},
			{Address(0x08, 0x00, 0x00), 256, 256, true, true},
			{Address(0x10, 0x00, 0x00), 23, 1, true, false},
			{Address(0x20, 0x00, 0x00), 0x4013, 1, false, false},
			{Address(0x7F, 0x00, 0x00), 0x3FFF, 1, false, false},
		};
		for (const Region& r : Regions)
			if (a >= r.Start && a < r.Start + r.EntrySize * r.Entries)
				return &r;
		return nullptr;
	}

	void ReadMunt(uint32_t a, size_t n, uint8_t* pOut) override
	{
		++Reads;
		const Region* r = Find(a);
		if (!r)
			return;
		const size_t nEnd = r->Start + r->EntrySize * r->Entries;
		if (a + n > nEnd)
			n = nEnd - a; // Munt clamps at the zone's end: the rest stays as it was
		for (size_t i = 0; i < n; ++i)
			pOut[i] = r->Readable ? Memory[a + i] : (i % 2 ? 0x06 : 0xFF);
	}

	void SendDT1(const uint8_t* p, size_t n) override
	{
		Sent.emplace_back(p, p + n);
		// F0 41 10 16 12 a a a data cs F7
		if (n < 11 || p[0] != 0xF0 || p[4] != 0x12 || !wire::ChecksumOK(p + 5, n - 6) || p[n - 1] != 0xF7)
		{
			++BadMessages;
			return;
		}
		uint32_t a = wire::Get21(p + 5);
		const uint8_t* d = p + 8;
		size_t nLen = n - 10;
		for (;;)
		{
			const Region* r = Find(a);
			if (!r)
				break;
			const uint32_t nEnd  = r->Start + r->EntrySize * r->Entries;
			const size_t nHere   = a + nLen > nEnd ? nEnd - a : nLen;
			const uint32_t nMove = r->Timbres ? 128 * 256 : 0; // writes go past the ROM timbres
			if (r->Readable)
				std::memcpy(&Memory[a + nMove], d, nHere);
			if (nHere == nLen)
				break;
			a += static_cast<uint32_t>(nHere);
			d += nHere;
			nLen -= nHere;
		}
	}

	std::vector<uint8_t> Memory;
	std::vector<std::vector<uint8_t>> Sent;
	int Reads = 0, BadMessages = 0;
};

static std::vector<uint8_t> Bytes(size_t n, unsigned nSeed)
{
	std::vector<uint8_t> v(n);
	for (size_t i = 0; i < n; ++i)
		v[i] = static_cast<uint8_t>((i * 13 + nSeed * 7 + 3) & 0x7F);
	return v;
}

static void TestAreas()
{
	CMuntModel Munt;
	std::vector<uint8_t> Out;
	size_t nBad = 99;

	// Patch memory: written with a DT1, read back
	auto Data = Bytes(40, 1);
	CHECK(mt32mem::Write(Munt, Address(0x05, 0x01, 0x00), Data.data(), Data.size(), nBad) == param::OK);
	CHECK(Munt.Sent.size() == 1 && Munt.BadMessages == 0);
	CHECK(mt32mem::Read(Munt, param::MT32, Address(0x05, 0x01, 0x00), Data.size(), Out) == param::OK && Out == Data);

	// Timbre memory: memory timbre 1 at 08 02 00, written there and read
	// back there, though Munt keeps it as its timbre 129
	Data = Bytes(246, 2);
	CHECK(mt32mem::Write(Munt, Address(0x08, 0x02, 0x00), Data.data(), Data.size(), nBad) == param::OK);
	CHECK(std::memcmp(&Munt.Memory[Address(0x08, 0x00, 0x00) + 129 * 256], Data.data(), Data.size()) == 0);
	CHECK(mt32mem::Read(Munt, param::MT32, Address(0x08, 0x02, 0x00), 246, Out) == param::OK && Out == Data);
	// The last byte of the timbre memory, and the first past it
	CHECK(mt32mem::Read(Munt, param::MT32, Address(0x08, 0x7F, 0x7F), 1, Out) == param::OK);
	CHECK(mt32mem::Read(Munt, param::MT32, Address(0x08, 0x7F, 0x7F), 2, Out) == param::BadAddress);
	CHECK(mt32mem::Write(Munt, Address(0x09, 0x00, 0x00), Data.data(), 1, nBad) == param::BadAddress && nBad == 0);

	// Patch temporary and rhythm setup are next to each other: one read
	// across both, in two calls to Munt (one per zone)
	Munt.Reads = 0;
	CHECK(mt32mem::Read(Munt, param::MT32, Address(0x03, 0x01, 0x00), 32, Out) == param::OK && Out.size() == 32);
	CHECK(Munt.Reads == 2);
	CHECK(std::memcmp(Out.data(), &Munt.Memory[Address(0x03, 0x01, 0x00)], 32) == 0);
	// The rhythm setup ends at 03 03 63: one more byte is outside the map
	CHECK(mt32mem::Read(Munt, param::MT32, Address(0x03, 0x03, 0x63), 1, Out) == param::OK);
	CHECK(mt32mem::Read(Munt, param::MT32, Address(0x03, 0x03, 0x63), 2, Out) == param::BadAddress && Out.empty());
	// System: 23 bytes
	CHECK(mt32mem::Read(Munt, param::MT32, Address(0x10, 0x00, 0x00), 23, Out) == param::OK);
	CHECK(mt32mem::Read(Munt, param::MT32, Address(0x10, 0x00, 0x00), 24, Out) == param::BadAddress);
	// Display and reset: written, never read
	CHECK(mt32mem::Read(Munt, param::MT32, Address(0x20, 0x00, 0x00), 4, Out) == param::BadAddress);
	CHECK(mt32mem::Read(Munt, param::MT32, Address(0x7F, 0x00, 0x00), 1, Out) == param::BadAddress);
	const std::string Text = "Golem is here";
	const size_t nBefore = Munt.Sent.size();
	CHECK(mt32mem::Write(Munt, Address(0x20, 0x00, 0x00), reinterpret_cast<const uint8_t*>(Text.data()), Text.size(),
	                     nBad) == param::OK);
	CHECK(Munt.Sent.size() == nBefore + 1);

	// A write that runs out of the map: refused at the first byte outside,
	// nothing sent
	Data = Bytes(30, 3);
	CHECK(mt32mem::Write(Munt, Address(0x10, 0x00, 0x00), Data.data(), Data.size(), nBad) == param::BadAddress &&
	      nBad == 23);
	CHECK(Munt.Sent.size() == nBefore + 1);
	// Outside the map altogether, and an area that is not the MT-32's
	CHECK(mt32mem::Read(Munt, param::MT32, Address(0x06, 0x00, 0x00), 1, Out) == param::BadAddress);
	CHECK(mt32mem::Read(Munt, param::OPL3Instr, 0, 1, Out) == param::BadAddress);

	// Area 04: A1 is Munt's timbre 0, B64 its 127, the first rhythm timbre
	// its 192
	const uint32_t T = Address(0x08, 0x00, 0x00);
	CHECK(mt32mem::Read(Munt, param::MT32ROM, 0, 256, Out) == param::OK &&
	      std::memcmp(Out.data(), &Munt.Memory[T], 256) == 0);
	CHECK(mt32mem::Read(Munt, param::MT32ROM, 127 * 256, 246, Out) == param::OK &&
	      std::memcmp(Out.data(), &Munt.Memory[T + 127 * 256], 246) == 0);
	CHECK(mt32mem::Read(Munt, param::MT32ROM, 128 * 256, 246, Out) == param::OK &&
	      std::memcmp(Out.data(), &Munt.Memory[T + 192 * 256], 246) == 0);
	// Across B64 and the first rhythm timbre: two places in Munt
	CHECK(mt32mem::Read(Munt, param::MT32ROM, 127 * 256 + 250, 12, Out) == param::OK && Out.size() == 12);
	CHECK(std::memcmp(Out.data(), &Munt.Memory[T + 127 * 256 + 250], 6) == 0 &&
	      std::memcmp(Out.data() + 6, &Munt.Memory[T + 192 * 256], 6) == 0);
	// 192 timbres, no more
	CHECK(mt32mem::Read(Munt, param::MT32ROM, 191 * 256, 256, Out) == param::OK);
	CHECK(mt32mem::Read(Munt, param::MT32ROM, 191 * 256, 257, Out) == param::BadAddress);
}

static void TestRQ1()
{
	CMuntModel Munt;
	auto RQ1 = [](uint32_t a, uint32_t n, uint8_t nDevice = 0x10) {
		std::vector<uint8_t> m = {0xF0, 0x41, nDevice, 0x16, 0x11};
		wire::Put21(m, a);
		wire::Put21(m, n);
		m.push_back(wire::Checksum(m.data() + 5, 6));
		m.push_back(0xF7);
		return m;
	};
	uint32_t a = 0, n = 0;

	auto m = RQ1(Address(0x08, 0x00, 0x00), 0x4000);
	CHECK(mt32mem::IsRQ1(m.data(), m.size(), a, n) && a == Address(0x08, 0x00, 0x00) && n == 0x4000);
	auto Bad = m;
	Bad[11] ^= 1; // checksum
	CHECK(!mt32mem::IsRQ1(Bad.data(), Bad.size(), a, n));
	auto Other = RQ1(Address(0x03, 0x00, 0x00), 16, 0x11); // unit 18
	CHECK(!mt32mem::IsRQ1(Other.data(), Other.size(), a, n));
	auto Channel = RQ1(Address(0x00, 0x00, 0x00), 16, 0x01); // relative to channel 2
	CHECK(!mt32mem::IsRQ1(Channel.data(), Channel.size(), a, n));
	std::vector<uint8_t> DT1 = {0xF0, 0x41, 0x10, 0x16, 0x12, 0x10, 0, 0, 1, 0x6F, 0xF7};
	CHECK(!mt32mem::IsRQ1(DT1.data(), DT1.size(), a, n));

	// The whole timbre memory: 64 DT1 of 256 bytes, in order, each one right
	auto Data = Bytes(246, 4);
	size_t nBad = 0;
	mt32mem::Write(Munt, Address(0x08, 0x7E, 0x00), Data.data(), Data.size(), nBad); // memory timbre 64
	auto Replies = mt32mem::AnswerRQ1(Munt, Address(0x08, 0x00, 0x00), 0x4000);
	CHECK(Replies.size() == 64);
	bool bAllRight = true;
	for (size_t i = 0; i < Replies.size(); ++i)
	{
		const auto& r = Replies[i];
		bAllRight &= r.size() == 256 + 10 && r[0] == 0xF0 && r[1] == 0x41 && r[2] == 0x10 && r[3] == 0x16 &&
		             r[4] == 0x12 && wire::Get21(r.data() + 5) == Address(0x08, 0x00, 0x00) + i * 256 &&
		             wire::ChecksumOK(r.data() + 5, r.size() - 6) && r.back() == 0xF7;
	}
	CHECK(bAllRight);
	CHECK(Replies.size() == 64 && std::memcmp(Replies[63].data() + 8, Data.data(), Data.size()) == 0);

	// Cut at the end of the zone: system is 23 bytes
	Replies = mt32mem::AnswerRQ1(Munt, Address(0x10, 0x00, 0x10), 100);
	CHECK(Replies.size() == 1 && Replies[0].size() == 7 + 10);
	// A request that starts in patch temporary stays there (144 bytes)
	Replies = mt32mem::AnswerRQ1(Munt, Address(0x03, 0x00, 0x00), 1000);
	CHECK(Replies.size() == 1 && Replies[0].size() == 144 + 10);
	// Nothing for the display, the reset, outside the map or a length of 0
	CHECK(mt32mem::AnswerRQ1(Munt, Address(0x20, 0x00, 0x00), 20).empty());
	CHECK(mt32mem::AnswerRQ1(Munt, Address(0x7F, 0x00, 0x00), 1).empty());
	CHECK(mt32mem::AnswerRQ1(Munt, Address(0x06, 0x00, 0x00), 1).empty());
	CHECK(mt32mem::AnswerRQ1(Munt, Address(0x10, 0x00, 0x00), 0).empty());
}

static bool SameMemory(const CMuntModel& a, const CMuntModel& b)
{
	const struct
	{
		uint32_t Start;
		size_t Size;
	} Zones[] = {
		{Address(0x05, 0x00, 0x00), 1024},
		{Address(0x03, 0x01, 0x10), 340},
		{Address(0x10, 0x00, 0x00), 23},
	};
	for (const auto& z : Zones)
		if (std::memcmp(&a.Memory[z.Start], &b.Memory[z.Start], z.Size) != 0)
			return false;
	// Timbre memory: 246 bytes of each of Munt's timbres 128-191
	for (uint32_t t = 128; t < 192; ++t)
		if (std::memcmp(&a.Memory[Address(0x08, 0, 0) + t * 256], &b.Memory[Address(0x08, 0, 0) + t * 256], 246) != 0)
			return false;
	return true;
}

static void TestDump()
{
	CMuntModel From, To;
	// Change what a dump carries in the first one
	size_t nBad = 0;
	for (uint32_t t = 0; t < 64; t += 9)
	{
		auto Data = Bytes(246, t);
		mt32mem::Write(From, Address(0x08, 0x00, 0x00) + t * 256, Data.data(), Data.size(), nBad);
	}
	auto Data = Bytes(1024, 5);
	for (size_t i = 0; i < Data.size(); i += 256)
		mt32mem::Write(From, Address(0x05, 0x00, 0x00) + static_cast<uint32_t>(i), Data.data() + i, 256, nBad);
	Data = Bytes(23, 6);
	mt32mem::Write(From, Address(0x10, 0x00, 0x00), Data.data(), Data.size(), nBad);
	CHECK(!SameMemory(From, To));

	const auto File = mt32mem::Dump(From);
	// 64 timbres of 246 bytes, 4 + 2 + 1 messages for patches, rhythm, system
	CHECK(File.size() == 64 * (246 + 10) + 4 * (256 + 10) + (256 + 10) + (84 + 10) + (23 + 10));

	CHECK(mt32mem::LoadDump(To, File.data(), File.size(), nBad) == param::OK);
	CHECK(To.Sent.size() == 64 + 4 + 2 + 1 && To.BadMessages == 0);
	CHECK(SameMemory(From, To));

	// A broken checksum in the third message: nothing sent, its offset back
	CMuntModel Fresh;
	auto Broken = File;
	const size_t nThird = 2 * (246 + 10);
	Broken[nThird + 20] ^= 1;
	CHECK(mt32mem::LoadDump(Fresh, Broken.data(), Broken.size(), nBad) == param::BadValue && nBad == nThird);
	CHECK(Fresh.Sent.empty());
	// A DT1 outside the map
	auto Outside = mt32mem::MakeDT1(Address(0x06, 0x00, 0x00), Data.data(), 4);
	std::vector<uint8_t> Mixed(File.begin(), File.begin() + 256);
	Mixed.insert(Mixed.end(), Outside.begin(), Outside.end());
	CHECK(mt32mem::LoadDump(Fresh, Mixed.data(), Mixed.size(), nBad) == param::BadAddress && nBad == 256);
	// Another unit, another model, bytes after the end, nothing at all
	auto Unit = File;
	Unit[2] = 0x11;
	CHECK(mt32mem::LoadDump(Fresh, Unit.data(), 256, nBad) == param::BadValue && nBad == 0);
	auto Tail = std::vector<uint8_t>(File.begin(), File.begin() + 256);
	Tail.push_back(0x00);
	CHECK(mt32mem::LoadDump(Fresh, Tail.data(), Tail.size(), nBad) == param::BadValue && nBad == 256);
	CHECK(mt32mem::LoadDump(Fresh, nullptr, 0, nBad) == param::BadValue);
	CHECK(Fresh.Sent.empty());
}

// BLOB type 04 through the protocol, with a host that keeps the MT-32 model
class CHost : public ParamHost
{
public:
	ParamInfo Info() override { return ParamInfo(); }
	uint8_t AreaRead(uint8_t nArea, uint32_t nAddress, size_t nSize, std::vector<uint8_t>& out) override
	{
		return mt32mem::Read(Munt, nArea, nAddress, nSize, out);
	}
	uint8_t AreaWrite(uint8_t nArea, uint32_t nAddress, const uint8_t* p, size_t n, size_t& nBad) override
	{
		if (nArea != param::MT32)
			return param::BadAddress;
		return mt32mem::Write(Munt, nAddress, p, n, nBad);
	}
	uint8_t BlobCommit(uint8_t nType, uint8_t nTarget, std::vector<uint8_t>&& file) override
	{
		size_t nBad = 0;
		return nType == param::BlobMT32 && nTarget == 0 ? mt32mem::LoadDump(Munt, file.data(), file.size(), nBad) :
		                                                  static_cast<uint8_t>(param::Unavailable);
	}
	uint8_t BlobGet(uint8_t nType, uint8_t nOrigin, std::vector<uint8_t>& file) override
	{
		if (nType != param::BlobMT32 || nOrigin != 0)
			return param::Unavailable;
		file = mt32mem::Dump(Munt);
		return param::OK;
	}
	CMuntModel Munt;
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
	std::vector<std::vector<uint8_t>> Out;
	ParamProtocol Protocol(Host, [&Out](const uint8_t* p, size_t n) { Out.emplace_back(p, p + n); });
	auto Send = [&](const std::vector<uint8_t>& f) {
		Out.clear();
		Protocol.Handle(f.data(), f.size());
		return Out;
	};

	// PARAM_WRITE in area 00 (the system's reverb level), confirmed, then read
	auto r = Send(Request(0x01, param::Write, {param::MT32, 0x10, 0x00, 0x03, 5}, 1));
	CHECK(r.size() == 1 && r[0][7] == param::Ready);
	r = Send(Request(0x02, param::Read, {param::MT32, 0x10, 0x00, 0x03, 1, 0}));
	CHECK(r.size() == 1 && r[0][7] == param::Data && r[0].size() == 9 + 4 + 1 + 2 && r[0][13] == 5);
	// Area 04 cannot be written; the display cannot be read
	r = Send(Request(0x03, param::Write, {param::MT32ROM, 0, 0, 0, 1}, 1));
	CHECK(r.size() == 1 && r[0][7] == param::Error && r[0][9] == param::BadAddress);
	r = Send(Request(0x04, param::Read, {param::MT32, 0x20, 0x00, 0x00, 1, 0}));
	CHECK(r.size() == 1 && r[0][7] == param::Error && r[0][9] == param::BadAddress);

	// The dump down (size, then chunks) and back up into a new model
	r = Send(Request(0x05, param::Blob, {0x04, param::BlobMT32, 0, 0, 0, 0, 0, 0}));
	CHECK(r.size() == 1 && r[0][7] == param::Data);
	const size_t nTotal = r.size() == 1 ? wire::Get21(r[0].data() + 10) : 0;
	CHECK(nTotal == 17841);
	std::vector<uint8_t> File;
	uint8_t tt = 0x06;
	while (File.size() < nTotal)
	{
		std::vector<uint8_t> b = {0x04, param::BlobMT32, 0};
		wire::Put21(b, static_cast<uint32_t>(File.size()));
		wire::Put14(b, 256);
		r = Send(Request(tt++ & 0x7F, param::Blob, b));
		if (r.size() != 1 || r[0][7] != param::Data)
		{
			CHECK(false);
			break;
		}
		std::vector<uint8_t> Part;
		CHECK(wire::Unpack(Part, r[0].data() + 13, r[0].size() - 15));
		File.insert(File.end(), Part.begin(), Part.end());
	}
	CHECK(File == mt32mem::Dump(Host.Munt));

	CHost Other;
	ParamProtocol Up(Other, [&Out](const uint8_t* p, size_t n) { Out.emplace_back(p, p + n); });
	auto SendUp = [&](const std::vector<uint8_t>& f) {
		Out.clear();
		Up.Handle(f.data(), f.size());
		return Out;
	};
	std::vector<uint8_t> Begin = {0x00, param::BlobMT32, 0};
	wire::Put21(Begin, static_cast<uint32_t>(File.size()));
	r = SendUp(Request(0x01, param::Blob, Begin));
	CHECK(r.size() == 1 && r[0][7] == param::Ready);
	for (size_t i = 0, nSeq = 0; i < File.size(); i += 256, ++nSeq)
	{
		std::vector<uint8_t> b = {0x01};
		wire::Put14(b, static_cast<unsigned>(nSeq));
		wire::Pack(b, File.data() + i, std::min<size_t>(256, File.size() - i));
		SendUp(Request(static_cast<uint8_t>(nSeq & 0x7F), param::Blob, b));
	}
	r = SendUp(Request(0x02, param::Blob, {0x02}));
	CHECK(r.size() == 2 && r[0][7] == param::Accepted && r[1][7] == param::Ready);
	CHECK(SameMemory(Host.Munt, Other.Munt));
}

int main()
{
	TestAreas();
	TestRQ1();
	TestDump();
	TestProtocol();
	std::printf("%s\n", g_nFailures ? "FAILED" : "all checks passed");
	return g_nFailures ? 1 : 0;
}
