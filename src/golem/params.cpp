//
// params.cpp
//
// Golem - Copyright (C) 2026 Javier Aguilar Saavedra (shared with golem-linux)
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "golem/params.h"

#include <algorithm>


#include "golem/wire.h"

namespace golem
{

namespace
{
constexpr uint8_t Header[] = {0xF0, 0x7D, 0x47, 0x4C, 0x4D, 0x01};
constexpr size_t HeaderSize = 8; // through the command
constexpr uint8_t FlagConfirm = 0x01;
constexpr size_t MaxBlob = 512 * 1024;
constexpr uint16_t NoDetail = 0x3FFF;

enum BlobOp : uint8_t
{
	Begin = 0x00,
	Chunk = 0x01,
	End   = 0x02,
	Abort = 0x03,
	Get   = 0x04,
};
} // namespace

void ParamProtocol::Reply(uint8_t tt, uint8_t nResponse, uint8_t nCommand, const std::vector<uint8_t>& payload,
                          bool bChecksum)
{
	std::vector<uint8_t> Frame(Header, Header + sizeof(Header));
	Frame.push_back(tt & 0x7F);
	Frame.push_back(nResponse);
	Frame.push_back(nCommand & 0x7F);
	for (const uint8_t b : payload)
		Frame.push_back(b & 0x7F);
	if (bChecksum)
		Frame.push_back(wire::Checksum(Frame.data() + 9, Frame.size() - 9));
	Frame.push_back(0xF7);
	m_Send(Frame.data(), Frame.size());
}

// ERROR of the new commands: code and detail, the position of the first
// refused byte within the data (14 bits; 3FFF: none)
void ParamProtocol::Fail(uint8_t tt, uint8_t nCommand, uint8_t nError, size_t nDetail)
{
	std::vector<uint8_t> Payload = {nError};
	wire::Put14(Payload, nDetail > NoDetail ? NoDetail : static_cast<unsigned>(nDetail));
	Reply(tt, param::Error, nCommand, Payload, false);
}

void ParamProtocol::Handle(const uint8_t* pData, size_t nSize)
{
	const uint8_t tt = pData[6], cc = pData[7];

	// flags ... cs between the header and F7
	if (nSize < HeaderSize + 3)
	{
		Fail(tt, cc, param::Malformed);
		return;
	}
	const uint8_t* pBody = pData + HeaderSize;
	const size_t nBody   = nSize - HeaderSize - 1;
	if (!wire::ChecksumOK(pBody, nBody))
	{
		Fail(tt, cc, param::BadChecksum);
		return;
	}
	const uint8_t nFlags = pBody[0];
	if (nFlags & ~FlagConfirm)
	{
		Fail(tt, cc, param::BadValue, 0);
		return;
	}
	const uint8_t* p = pBody + 1;
	const size_t n   = nBody - 2; // without flags and cs

	switch (cc)
	{
		case param::GetInfo: HandleInfo(tt, p, n); return;
		case param::Write: HandleWrite(tt, nFlags, p, n); return;
		case param::Read: HandleRead(tt, p, n); return;
		case param::Blob: HandleBlob(tt, nFlags, p, n); return;
		default: Fail(tt, cc, param::UnknownCommand); return;
	}
}

void ParamProtocol::HandleInfo(uint8_t tt, const uint8_t* p, size_t n)
{
	if (n != 1)
	{
		Fail(tt, param::GetInfo, param::Malformed);
		return;
	}
	const ParamInfo i = m_Host.Info();
	std::vector<uint8_t> Out = {p[0]};
	if (p[0] == 0)
	{
		Out.push_back(1); // protocol revision 1.1
		Out.push_back(1);
		Out.push_back(i.Platform);
		Out.insert(Out.end(), {i.Version[0], i.Version[1], i.Version[2]});
		Out.push_back(i.Engines);
		wire::Put14(Out, i.Functions);
		wire::Put14(Out, i.MaxSysEx);
		wire::Put14(Out, param::MaxData);
		Out.push_back(param::BlobWindow);
		Out.push_back(i.OPL3Chips);
		Out.push_back(i.FourOpChannels);
		Out.push_back(i.Slots);
		wire::Put14(Out, i.SoundFonts);
		Out.push_back(i.OPL3Banks);
	}
	else if (p[0] == 1)
	{
		for (const char c : i.Name)
			Out.push_back(static_cast<uint8_t>(c) & 0x7F);
	}
	else
	{
		Fail(tt, param::GetInfo, param::BadValue, 0);
		return;
	}
	Reply(tt, param::Info, param::GetInfo, Out, true);
}

void ParamProtocol::HandleWrite(uint8_t tt, uint8_t nFlags, const uint8_t* p, size_t n)
{
	// area, address (3), data (1-256)
	if (n < 5)
	{
		Fail(tt, param::Write, param::Malformed);
		return;
	}
	const size_t nData = n - 4;
	if (nData > param::MaxData)
	{
		Fail(tt, param::Write, param::TooLong);
		return;
	}
	size_t nBad = NoDetail;
	const uint8_t nError = m_Host.AreaWrite(p[0], wire::Get21(p + 1), p + 4, nData, nBad);
	if (nError != param::OK)
		Fail(tt, param::Write, nError, nBad);
	else if (nFlags & FlagConfirm)
		Reply(tt, param::Ready, param::Write, {}, false);
}

void ParamProtocol::HandleRead(uint8_t tt, const uint8_t* p, size_t n)
{
	// area, address (3), length (14 bits)
	if (n != 6)
	{
		Fail(tt, param::Read, param::Malformed);
		return;
	}
	const size_t nLength = wire::Get14(p + 4);
	if (nLength == 0 || nLength > param::MaxData)
	{
		Fail(tt, param::Read, static_cast<uint8_t>(nLength ? param::TooLong : param::BadValue), 4);
		return;
	}
	std::vector<uint8_t> Out(p, p + 4); // area and address
	std::vector<uint8_t> Data;
	const uint8_t nError = m_Host.AreaRead(p[0], wire::Get21(p + 1), nLength, Data);
	if (nError != param::OK || Data.size() != nLength)
	{
		Fail(tt, param::Read, nError != param::OK ? nError : static_cast<uint8_t>(param::Failed));
		return;
	}
	Out.insert(Out.end(), Data.begin(), Data.end());
	Reply(tt, param::Data, param::Read, Out, true);
}

void ParamProtocol::HandleBlob(uint8_t tt, uint8_t nFlags, const uint8_t* p, size_t n)
{
	if (n < 1)
	{
		Fail(tt, param::Blob, param::Malformed);
		return;
	}
	const uint8_t nOp = p[0];
	++p;
	--n;

	switch (nOp)
	{
		case Begin:
		{
			// type, target, size (21 bits)
			if (n != 5)
			{
				Fail(tt, param::Blob, param::Malformed);
				return;
			}
			const size_t nTotal = wire::Get21(p + 2);
			if (p[0] != param::BlobWOPL && p[0] != param::BlobFX && p[0] != param::BlobMT32)
			{
				Fail(tt, param::Blob, param::Unavailable, 1);
				return;
			}
			if (p[1] != 0)
			{
				Fail(tt, param::Blob, param::Unavailable, 2); // slots: not yet
				return;
			}
			if (nTotal == 0 || nTotal > MaxBlob)
			{
				Fail(tt, param::Blob, param::TooLong, 3);
				return;
			}
			m_Upload = Upload{true, p[0], p[1], nTotal, 0, {}};
			m_Upload.Data.reserve(nTotal);
			Reply(tt, param::Ready, param::Blob, {Begin}, false);
			return;
		}

		case Chunk:
		{
			if (!m_Upload.Open || n < 3)
			{
				Fail(tt, param::Blob, m_Upload.Open ? param::Malformed : param::Sequence);
				return;
			}
			const uint16_t nSequence = wire::Get14(p);
			if (nSequence != m_Upload.NextSequence)
			{
				Fail(tt, param::Blob, param::Sequence, 1);
				return;
			}
			std::vector<uint8_t> Chunk;
			if (!wire::Unpack(Chunk, p + 2, n - 2))
			{
				Fail(tt, param::Blob, param::Malformed, 3);
				return;
			}
			if (Chunk.size() > param::MaxData)
			{
				Fail(tt, param::Blob, param::TooLong, 3);
				return;
			}
			if (m_Upload.Data.size() + Chunk.size() > m_Upload.Size)
			{
				m_Upload.Open = false;
				Fail(tt, param::Blob, param::Sequence, 3);
				return;
			}
			m_Upload.Data.insert(m_Upload.Data.end(), Chunk.begin(), Chunk.end());
			m_Upload.NextSequence = (m_Upload.NextSequence + 1) & 0x3FFF;
			if (nFlags & FlagConfirm)
			{
				std::vector<uint8_t> Out = {nOp};
				wire::Put14(Out, nSequence);
				Reply(tt, param::Ready, param::Blob, Out, false);
			}
			return;
		}

		case End:
		{
			if (!m_Upload.Open || m_Upload.Data.size() != m_Upload.Size)
			{
				m_Upload.Open = false;
				Fail(tt, param::Blob, param::Sequence);
				return;
			}
			m_Upload.Open = false;
			m_Download.Valid = false;
			Reply(tt, param::Accepted, param::Blob, {}, false);
			const uint8_t nError = m_Host.BlobCommit(m_Upload.Type, m_Upload.Target, std::move(m_Upload.Data));
			m_Upload.Data.clear();
			if (nError != param::OK)
				Fail(tt, param::Blob, nError);
			else
				Reply(tt, param::Ready, param::Blob, {End}, false);
			return;
		}

		case Abort:
			m_Upload = Upload{};
			Reply(tt, param::Ready, param::Blob, {Abort}, false);
			return;

		case Get:
		{
			// type, origin, position (21 bits), length (14 bits)
			if (n != 7)
			{
				Fail(tt, param::Blob, param::Malformed);
				return;
			}
			const uint8_t nType = p[0], nOrigin = p[1];
			const size_t nPosition = wire::Get21(p + 2);
			const size_t nLength   = wire::Get14(p + 5);
			if (nLength > param::MaxData)
			{
				Fail(tt, param::Blob, param::TooLong, 6);
				return;
			}
			// A fresh copy when a read starts over (position 0 or the size
			// query), so a long read sees one consistent file
			if (!m_Download.Valid || m_Download.Type != nType || m_Download.Origin != nOrigin || nPosition == 0)
			{
				m_Download = Download{};
				const uint8_t nError = m_Host.BlobGet(nType, nOrigin, m_Download.Data);
				if (nError != param::OK)
				{
					Fail(tt, param::Blob, nError, 1);
					return;
				}
				m_Download.Valid  = true;
				m_Download.Type   = nType;
				m_Download.Origin = nOrigin;
			}
			const size_t nTotal = m_Download.Data.size();
			std::vector<uint8_t> Out = {Get};
			if (nLength == 0)
			{
				wire::Put21(Out, static_cast<uint32_t>(nTotal));
				Reply(tt, param::Data, param::Blob, Out, true);
				return;
			}
			if (nPosition >= nTotal)
			{
				Fail(tt, param::Blob, param::BadAddress, 2);
				return;
			}
			const size_t nSlice = std::min(nLength, nTotal - nPosition);
			wire::Put21(Out, static_cast<uint32_t>(nPosition));
			wire::Pack(Out, m_Download.Data.data() + nPosition, nSlice);
			Reply(tt, param::Data, param::Blob, Out, true);
			return;
		}

		default: Fail(tt, param::Blob, param::BadValue, 0); return;
	}
}

} // namespace golem
