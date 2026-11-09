//
// params.h - sound-parameter protocol: Golem commands 07 onwards
//
// Golem - Copyright (C) 2026 Javier Aguilar Saavedra (shared with golem-linux)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// golem-next docs/sound-params.md. Same frame and version as the control
// protocol v1; every new command starts with a flags byte
// and ends with a Roland checksum. Implemented here: GET_INFO (07),
// PARAM_WRITE (08), PARAM_READ (09) and BLOB (0C) for WOPL/WOPLX banks,
// FluidSynth effects profiles and MT-32 dumps.
// OPL3_REG, OPL3_SESSION and SLOT answer "unknown command" for now.
//

#ifndef GOLEM_PARAMS_H
#define GOLEM_PARAMS_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace golem
{

namespace param
{
enum Command : uint8_t
{
	GetInfo    = 0x07,
	Write      = 0x08,
	Read       = 0x09,
	OPL3Reg    = 0x0A,
	OPL3Session = 0x0B,
	Blob       = 0x0C,
	Slot       = 0x0D,
	Notify     = 0x0E,
};

enum Response : uint8_t
{
	Accepted = 0x40,
	Ready    = 0x41,
	Error    = 0x42,
	Info     = 0x44,
	Data     = 0x45,
};

enum ErrorCode : uint8_t
{
	OK             = 0x00,
	Malformed      = 0x01,
	UnknownCommand = 0x03,
	BadValue       = 0x04,
	Unavailable    = 0x05,
	Failed         = 0x06,
	BadChecksum    = 0x07,
	BadAddress     = 0x08,
	TooLong        = 0x09,
	Busy           = 0x0A,
	Sequence       = 0x0B,
};

enum Area : uint8_t
{
	MT32        = 0x00,
	OPL3Instr   = 0x01,
	OPL3Banks   = 0x02,
	OPL3Regs    = 0x03,
	MT32ROM     = 0x04,
	Settings    = 0x10,
};

enum BlobType : uint8_t
{
	BlobWOPL = 0x01,
	BlobFX   = 0x03, // FluidSynth effects profile (.cfg text)
	BlobMT32 = 0x04, // MT-32 dump: DT1 messages (.SYX)
};

// GET_INFO bits 7-8 (sound-params.md 4.1)
enum Function : uint16_t
{
	FnAreas      = 1 << 0,
	FnMT32RQ1    = 1 << 1,
	FnOPL3Instr  = 1 << 2,
	FnOPL3Regs   = 1 << 3,
	FnWOPLUp     = 1 << 4,
	FnWOPLDown   = 1 << 5,
	FnSlots      = 1 << 6,
	FnNotify     = 1 << 7,
	FnROMTimbres = 1 << 9,
};

constexpr size_t MaxData = 256;   // bytes per PARAM_WRITE, DATA and BLOB chunk (decoded)
constexpr uint8_t BlobWindow = 4; // BLOB chunks a client may send ahead
} // namespace param

struct ParamInfo
{
	uint8_t Platform = 1; // 0 Circle firmware, 1 golemd
	uint8_t Version[3] = {0, 0, 0};
	uint8_t Engines = 0; // as STATUS
	uint16_t Functions = 0;
	uint16_t MaxSysEx = 1000;
	uint8_t OPL3Chips = 0;
	uint8_t FourOpChannels = 0;
	uint8_t Slots = 0;
	uint16_t SoundFonts = 0;
	uint8_t OPL3Banks = 0;
	std::string Name;
};

// What the parameter commands need from the engine. Error codes are
// param::ErrorCode; on BadValue or BadAddress, nBad is the position of the
// first refused byte in the data.
class ParamHost
{
public:
	virtual ~ParamHost() = default;
	virtual ParamInfo Info() = 0;
	virtual uint8_t AreaRead(uint8_t nArea, uint32_t nAddress, size_t nSize, std::vector<uint8_t>& out) = 0;
	virtual uint8_t AreaWrite(uint8_t nArea, uint32_t nAddress, const uint8_t* pData, size_t nSize, size_t& nBad) = 0;
	// A whole file in (BLOB END) and out (BLOB READ)
	virtual uint8_t BlobCommit(uint8_t nType, uint8_t nTarget, std::vector<uint8_t>&& file) = 0;
	virtual uint8_t BlobGet(uint8_t nType, uint8_t nOrigin, std::vector<uint8_t>& file) = 0;
};

class ParamProtocol
{
public:
	using SendFn = std::function<void(const uint8_t* data, size_t size)>;

	ParamProtocol(ParamHost& host, SendFn send) : m_Host(host), m_Send(std::move(send)) {}

	// A whole Golem frame (F0 7D 47 4C 4D 01 tt cc ... F7) whose command is
	// 07 or more
	void Handle(const uint8_t* pData, size_t nSize);

private:
	void Reply(uint8_t nTransaction, uint8_t nResponse, uint8_t nCommand, const std::vector<uint8_t>& payload,
	           bool bChecksum);
	void Fail(uint8_t nTransaction, uint8_t nCommand, uint8_t nError, size_t nDetail = 0x3FFF); // 3FFF: no detail

	void HandleInfo(uint8_t tt, const uint8_t* p, size_t n);
	void HandleWrite(uint8_t tt, uint8_t nFlags, const uint8_t* p, size_t n);
	void HandleRead(uint8_t tt, const uint8_t* p, size_t n);
	void HandleBlob(uint8_t tt, uint8_t nFlags, const uint8_t* p, size_t n);

	ParamHost& m_Host;
	SendFn m_Send;

	// One BLOB at a time: replies go only to the Next's UART for now
	struct Upload
	{
		bool Open = false;
		uint8_t Type = 0, Target = 0;
		size_t Size = 0;
		uint16_t NextSequence = 0;
		std::vector<uint8_t> Data;
	} m_Upload;
	// The file being read out, kept between chunks
	struct Download
	{
		bool Valid = false;
		uint8_t Type = 0, Origin = 0;
		std::vector<uint8_t> Data;
	} m_Download;
};

} // namespace golem

#endif
