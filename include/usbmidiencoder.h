//
// usbmidiencoder.h
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
// Copyright (C) 2020-2023 Dale Whinham <daleyo@gmail.com>
//
// This file is part of mt32-pi.
//
// mt32-pi is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// mt32-pi is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along with
// mt32-pi. If not, see <http://www.gnu.org/licenses/>.
//

#ifndef _usbmidiencoder_h
#define _usbmidiencoder_h

// Encodes complete MIDI messages, as delivered by CMIDIParser, into USB-MIDI
// 1.0 event packets (four bytes each: cable/CIN, then up to three MIDI bytes).
// Kept free of Circle dependencies so it can be unit-tested on the host.

#include <stddef.h>
#include <stdint.h>

namespace USBMIDI
{
	constexpr size_t PacketSize = 4;

	// Bytes needed to encode a SysEx message of nSize bytes (F0 ... F7)
	constexpr size_t SysExEncodedSize(size_t nSize) { return (nSize + 2) / 3 * PacketSize; }

	// Encode a short message packed as status | data1 << 8 | data2 << 16.
	// Writes one packet to pOut and returns 4, or returns 0 if the status is
	// not something CMIDIParser emits as a short message.
	inline size_t EncodeShortMessage(uint32_t nMessage, uint8_t nCable, uint8_t* pOut)
	{
		const uint8_t nStatus = nMessage & 0xFF;
		const uint8_t nData1  = (nMessage >> 8) & 0x7F;
		const uint8_t nData2  = (nMessage >> 16) & 0x7F;

		uint8_t nCIN;
		size_t nLength;

		if (nStatus >= 0x80 && nStatus < 0xF0)
		{
			nCIN    = nStatus >> 4;
			nLength = (nStatus >= 0xC0 && nStatus < 0xE0) ? 2 : 3;
		}
		else
		{
			switch (nStatus)
			{
				case 0xF1:
				case 0xF3:
					nCIN    = 0x2;
					nLength = 2;
					break;

				case 0xF2:
					nCIN    = 0x3;
					nLength = 3;
					break;

				case 0xF6:
					nCIN    = 0x5;
					nLength = 1;
					break;

				case 0xF8:
				case 0xFA:
				case 0xFB:
				case 0xFC:
				case 0xFE:
				case 0xFF:
					nCIN    = 0xF;
					nLength = 1;
					break;

				default:
					return 0;
			}
		}

		pOut[0] = static_cast<uint8_t>((nCable & 0x0F) << 4 | nCIN);
		pOut[1] = nStatus;
		pOut[2] = nLength > 1 ? nData1 : 0;
		pOut[3] = nLength > 2 ? nData2 : 0;
		return PacketSize;
	}

	// Encode a complete SysEx message (starting with F0 and ending with F7).
	// pOut must hold SysExEncodedSize(nSize) bytes. Returns the bytes written,
	// or 0 if the message is not a complete SysEx.
	inline size_t EncodeSysEx(const uint8_t* pData, size_t nSize, uint8_t nCable, uint8_t* pOut)
	{
		if (nSize < 2 || pData[0] != 0xF0 || pData[nSize - 1] != 0xF7)
			return 0;

		const uint8_t nCableBits = static_cast<uint8_t>((nCable & 0x0F) << 4);
		size_t nWritten = 0;

		while (nSize > 0)
		{
			const size_t nChunk = nSize > 3 ? 3 : nSize;

			// CIN 4: SysEx starts or continues; CIN 5/6/7: SysEx ends with 1/2/3 bytes
			const uint8_t nCIN = nSize > 3 ? 0x4 : static_cast<uint8_t>(0x4 + nChunk);

			pOut[nWritten++] = nCableBits | nCIN;
			for (size_t i = 0; i < 3; ++i)
				pOut[nWritten++] = i < nChunk ? pData[i] : 0;

			pData += nChunk;
			nSize -= nChunk;
		}

		return nWritten;
	}

	// Single-producer, single-consumer byte FIFO of whole packets, used only
	// from the main task. Push() is all-or-nothing so a message is never
	// split by a full queue.
	template <size_t N>
	class CPacketQueue
	{
	public:
		static_assert(N % PacketSize == 0, "Queue size must be a multiple of the packet size");

		CPacketQueue() : m_nHead(0), m_nCount(0), m_nDropped(0) {}

		size_t GetFree() const { return N - m_nCount; }
		size_t GetCount() const { return m_nCount; }
		size_t GetDropped() const { return m_nDropped; }
		void Clear() { m_nHead = m_nCount = 0; }

		bool Push(const uint8_t* pData, size_t nSize)
		{
			if (nSize > GetFree())
			{
				++m_nDropped;
				return false;
			}

			size_t nTail = (m_nHead + m_nCount) % N;
			for (size_t i = 0; i < nSize; ++i)
			{
				m_Data[nTail] = pData[i];
				nTail = (nTail + 1) % N;
			}

			m_nCount += nSize;
			return true;
		}

		// Copy out up to nMaxSize bytes (rounded down to whole packets)
		size_t Pop(uint8_t* pOut, size_t nMaxSize)
		{
			size_t nSize = nMaxSize < m_nCount ? nMaxSize : m_nCount;
			nSize -= nSize % PacketSize;

			for (size_t i = 0; i < nSize; ++i)
			{
				pOut[i] = m_Data[m_nHead];
				m_nHead = (m_nHead + 1) % N;
			}

			m_nCount -= nSize;
			return nSize;
		}

	private:
		uint8_t m_Data[N];
		size_t m_nHead;
		size_t m_nCount;
		size_t m_nDropped;
	};
}

#endif
