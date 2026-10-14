// Host unit test for include/usbmidiencoder.h
// Build: g++ -std=c++17 -Wall -Wextra -Werror -Iinclude tests/test_usbmidiencoder.cpp -o test_usbmidiencoder

#include <cstdio>
#include <cstring>
#include <vector>

#include "usbmidiencoder.h"

static int g_nFailures = 0;

static void Expect(const char* pName, const std::vector<uint8_t>& Got, const std::vector<uint8_t>& Want)
{
	if (Got == Want)
	{
		std::printf("PASS %s\n", pName);
		return;
	}

	++g_nFailures;
	std::printf("FAIL %s\n  got: ", pName);
	for (uint8_t b : Got)
		std::printf("%02X ", b);
	std::printf("\n want: ");
	for (uint8_t b : Want)
		std::printf("%02X ", b);
	std::printf("\n");
}

static std::vector<uint8_t> Short(uint32_t nMessage, uint8_t nCable = 0)
{
	uint8_t Packet[4] = {0xAA, 0xAA, 0xAA, 0xAA};
	const size_t n = USBMIDI::EncodeShortMessage(nMessage, nCable, Packet);
	return std::vector<uint8_t>(Packet, Packet + n);
}

static std::vector<uint8_t> SysEx(const std::vector<uint8_t>& Message, uint8_t nCable = 0)
{
	std::vector<uint8_t> Out(USBMIDI::SysExEncodedSize(Message.size()) + 4, 0xAA);
	const size_t n = USBMIDI::EncodeSysEx(Message.data(), Message.size(), nCable, Out.data());
	if (n > USBMIDI::SysExEncodedSize(Message.size()))
	{
		++g_nFailures;
		std::printf("FAIL encoded size larger than SysExEncodedSize\n");
	}
	Out.resize(n);
	return Out;
}

int main()
{
	// Channel voice messages: status | d1 << 8 | d2 << 16, as CMIDIParser packs them
	Expect("note on ch2", Short(0x91 | 60 << 8 | 100 << 16), {0x09, 0x91, 60, 100});
	Expect("note off ch10", Short(0x89 | 36 << 8), {0x08, 0x89, 36, 0});
	Expect("control change cable 1", Short(0xB0 | 7 << 8 | 127 << 16, 1), {0x1B, 0xB0, 7, 127});
	Expect("program change two bytes", Short(0xC1 | 5 << 8 | 0x55 << 16), {0x0C, 0xC1, 5, 0});
	Expect("channel pressure two bytes", Short(0xD3 | 64 << 8), {0x0D, 0xD3, 64, 0});
	Expect("pitch bend", Short(0xE0 | 0x00 << 8 | 0x40 << 16), {0x0E, 0xE0, 0x00, 0x40});

	// System common
	Expect("time code", Short(0xF1 | 0x23 << 8), {0x02, 0xF1, 0x23, 0});
	Expect("song position", Short(0xF2 | 0x10 << 8 | 0x02 << 16), {0x03, 0xF2, 0x10, 0x02});
	Expect("song select", Short(0xF3 | 0x04 << 8), {0x02, 0xF3, 0x04, 0});
	Expect("tune request", Short(0xF6), {0x05, 0xF6, 0, 0});

	// System real-time
	Expect("clock", Short(0xF8), {0x0F, 0xF8, 0, 0});
	Expect("start", Short(0xFA), {0x0F, 0xFA, 0, 0});
	Expect("active sensing", Short(0xFE), {0x0F, 0xFE, 0, 0});
	Expect("reset", Short(0xFF), {0x0F, 0xFF, 0, 0});

	// Not emitted by the parser: rejected
	Expect("undefined F4", Short(0xF4), {});
	Expect("bare data byte", Short(0x40), {});

	// SysEx: every length class of the final packet
	Expect("sysex 2 bytes", SysEx({0xF0, 0xF7}), {0x06, 0xF0, 0xF7, 0});
	Expect("sysex 3 bytes", SysEx({0xF0, 0x7D, 0xF7}), {0x07, 0xF0, 0x7D, 0xF7});
	Expect("sysex 4 bytes", SysEx({0xF0, 0x7E, 0x7F, 0xF7}), {0x04, 0xF0, 0x7E, 0x7F, 0x05, 0xF7, 0, 0});
	Expect("sysex 6 bytes GM on", SysEx({0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}),
	       {0x04, 0xF0, 0x7E, 0x7F, 0x07, 0x09, 0x01, 0xF7});
	Expect("sysex 11 bytes MT-32 cable 2",
	       SysEx({0xF0, 0x41, 0x10, 0x16, 0x12, 0x7F, 0x00, 0x00, 0x00, 0x01, 0xF7}, 2),
	       {0x24, 0xF0, 0x41, 0x10, 0x24, 0x16, 0x12, 0x7F, 0x24, 0x00, 0x00, 0x00, 0x26, 0x01, 0xF7, 0});
	Expect("sysex missing F7", SysEx({0xF0, 0x41, 0x10}), {});
	Expect("sysex missing F0", SysEx({0x41, 0x10, 0xF7}), {});

	// Largest SysEx the parser can deliver
	{
		std::vector<uint8_t> Big(1000, 0x11);
		Big.front() = 0xF0;
		Big.back()  = 0xF7;
		const std::vector<uint8_t> Encoded = SysEx(Big);
		bool bOK = Encoded.size() == 1336 && Encoded[0] == 0x04 && Encoded[1332] == 0x05 && Encoded[1333] == 0xF7;
		Expect("sysex 1000 bytes", {static_cast<uint8_t>(bOK)}, {1});
	}

	// Packet queue: all-or-nothing push, whole-packet pop, wrap-around
	{
		USBMIDI::CPacketQueue<16> Queue;
		const uint8_t A[8] = {1, 2, 3, 4, 5, 6, 7, 8};
		const uint8_t B[12] = {9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
		uint8_t Out[16];

		bool bOK = Queue.Push(A, 8) && !Queue.Push(B, 12) && Queue.GetDropped() == 1 && Queue.GetCount() == 8;
		Expect("queue rejects oversize push", {static_cast<uint8_t>(bOK)}, {1});

		size_t n = Queue.Pop(Out, 6);
		Expect("queue pops whole packets", std::vector<uint8_t>(Out, Out + n), {1, 2, 3, 4});

		bOK = Queue.Push(B, 12) && Queue.GetCount() == 16 && Queue.GetFree() == 0;
		Expect("queue fills after wrap", {static_cast<uint8_t>(bOK)}, {1});

		n = Queue.Pop(Out, sizeof(Out));
		Expect("queue order across wrap", std::vector<uint8_t>(Out, Out + n),
		       {5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20});

		Queue.Push(A, 4);
		Queue.Clear();
		Expect("queue clear", {static_cast<uint8_t>(Queue.GetCount())}, {0});
	}

	std::printf(g_nFailures ? "%d FAILED\n" : "all passed\n", g_nFailures);
	return g_nFailures ? 1 : 0;
}
