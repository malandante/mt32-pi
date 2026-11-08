//
// fxparams.h - FluidSynth's effects for the sound-parameter protocol
//
// Golem - Copyright (C) 2026 Javier Aguilar Saavedra (shared with golem-linux)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// golem-next docs/sound-params.md, section 9: area 10, section 02 (reverb,
// chorus, polyphony and gain of what FluidSynth plays) and the effects
// profile as a .cfg file (BLOB type 03), in mt32-pi's format (key = value,
// one per line) with two more keys, chorus_type and polyphony. The values are
// kept in the protocol's units, so what is written is read back unchanged.
//

#ifndef GOLEM_FXPARAMS_H
#define GOLEM_FXPARAMS_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace golem
{

struct FXSettings
{
	bool Reverb          = true;
	uint16_t RoomSize    = 200;  // x1000, 0-1000
	uint16_t Damping     = 0;    // x1000, 0-1000
	uint16_t Width       = 50;   // x100, 0-10000
	uint16_t Level       = 900;  // x1000, 0-1000
	bool Chorus          = true;
	uint8_t Voices       = 3;    // 0-99
	uint16_t ChorusLevel = 2000; // x1000, 0-10000
	uint16_t Speed       = 300;  // x1000 (Hz), 100-5000
	uint16_t Depth       = 80;   // x10 (ms), 0-2560
	uint8_t ChorusType   = 0;    // 0 sine, 1 triangle
	uint16_t Polyphony   = 200;  // 1-16383
	uint16_t Gain        = 200;  // x1000, 0-10000

	float RoomSizeF() const { return RoomSize / 1000.0f; }
	float DampingF() const { return Damping / 1000.0f; }
	float WidthF() const { return Width / 100.0f; }
	float LevelF() const { return Level / 1000.0f; }
	float ChorusLevelF() const { return ChorusLevel / 1000.0f; }
	float SpeedF() const { return Speed / 1000.0f; }
	float DepthF() const { return Depth / 10.0f; }
	float GainF() const { return Gain / 1000.0f; }

	bool operator==(const FXSettings& o) const;
};

namespace fxparams
{

constexpr uint32_t Last = 0x15; // the last parameter of section 02

// From floats as mt32-pi.cfg and the profiles give them; out of range values
// are clamped
uint16_t Units(float nValue, float nScale, unsigned nMin, unsigned nMax);

// One parameter byte (sound-params.md 9): param::OK or param::BadAddress
uint8_t Read(const FXSettings& FX, uint32_t nParameter, uint8_t& nValue);

// Bytes from nParameter on into a copy of FX, then checked whole; on a fault
// FX is unchanged, nBad is the position of the first refused byte and the
// result is param::BadAddress or param::BadValue
uint8_t Write(FXSettings& FX, uint32_t nParameter, const uint8_t* pData, size_t nSize, size_t& nBad);

// The profile as a .cfg file, and back. FromConfig starts from FX (so keys
// left out keep their values); on a fault FX is unchanged and nBadLine is
// the line (from 1) with an unknown key or a value out of range.
std::string ToConfig(const FXSettings& FX);
bool FromConfig(const char* pText, size_t nSize, FXSettings& FX, size_t& nBadLine);

} // namespace fxparams
} // namespace golem

#endif
