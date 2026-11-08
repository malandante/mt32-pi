//
// fxparams.cpp
//
// Golem - Copyright (C) 2026 Javier Aguilar Saavedra (shared with golem-linux)
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "golem/fxparams.h"

#include <cctype>
#include <math.h> // roundf, powf: newlib's <cmath> has no std::round
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "golem/params.h"

namespace golem
{

bool FXSettings::operator==(const FXSettings& o) const
{
	return Reverb == o.Reverb && RoomSize == o.RoomSize && Damping == o.Damping && Width == o.Width &&
	       Level == o.Level && Chorus == o.Chorus && Voices == o.Voices && ChorusLevel == o.ChorusLevel &&
	       Speed == o.Speed && Depth == o.Depth && ChorusType == o.ChorusType && Polyphony == o.Polyphony &&
	       Gain == o.Gain;
}

namespace fxparams
{

namespace
{
enum : uint32_t
{
	PReverb      = 0x00,
	PRoomSize    = 0x01,
	PDamping     = 0x03,
	PWidth       = 0x05,
	PLevel       = 0x07,
	PChorus      = 0x09,
	PVoices      = 0x0A,
	PChorusLevel = 0x0B,
	PSpeed       = 0x0D,
	PDepth       = 0x0F,
	PChorusType  = 0x11,
	PPolyphony   = 0x12,
	PGain        = 0x14,
};

// The 14-bit values: where each starts, the field and its range
struct Wide
{
	uint32_t First;
	uint16_t FXSettings::*Field;
	unsigned Min, Max;
	float Scale;
	const char* Key;
};

const Wide Wides[] = {
	{PRoomSize, &FXSettings::RoomSize, 0, 1000, 1000.0f, "reverb_room_size"},
	{PDamping, &FXSettings::Damping, 0, 1000, 1000.0f, "reverb_damping"},
	{PWidth, &FXSettings::Width, 0, 10000, 100.0f, "reverb_width"},
	{PLevel, &FXSettings::Level, 0, 1000, 1000.0f, "reverb_level"},
	{PChorusLevel, &FXSettings::ChorusLevel, 0, 10000, 1000.0f, "chorus_level"},
	{PSpeed, &FXSettings::Speed, 100, 5000, 1000.0f, "chorus_speed"},
	{PDepth, &FXSettings::Depth, 0, 2560, 10.0f, "chorus_depth"},
	{PPolyphony, &FXSettings::Polyphony, 1, 16383, 1.0f, "polyphony"},
	{PGain, &FXSettings::Gain, 0, 10000, 1000.0f, "gain"},
};

const Wide* FindWide(uint32_t p)
{
	for (const Wide& w : Wides)
		if (p == w.First || p == w.First + 1)
			return &w;
	return nullptr;
}

// "on", "true", "1" and their opposites, as mt32-pi.cfg takes them
bool ParseBool(const char* p, bool& b)
{
	if (!strcasecmp(p, "on") || !strcasecmp(p, "true") || !strcmp(p, "1"))
		b = true;
	else if (!strcasecmp(p, "off") || !strcasecmp(p, "false") || !strcmp(p, "0"))
		b = false;
	else
		return false;
	return true;
}

bool ParseFloat(const char* p, float& f)
{
	char* pEnd = nullptr;
	f = strtof(p, &pEnd);
	return pEnd != p && *pEnd == '\0' && __builtin_isfinite(f);
}

// A float in range, in the protocol's units
bool ParseUnits(const char* p, float nScale, unsigned nMin, unsigned nMax, uint16_t& v)
{
	float f;
	if (!ParseFloat(p, f))
		return false;
	const float u = roundf(f * nScale);
	if (u < static_cast<float>(nMin) || u > static_cast<float>(nMax))
		return false;
	v = static_cast<uint16_t>(u);
	return true;
}

void Trim(std::string& s)
{
	size_t a = 0, b = s.size();
	while (a < b && isspace(static_cast<unsigned char>(s[a])))
		++a;
	while (b > a && isspace(static_cast<unsigned char>(s[b - 1])))
		--b;
	s = s.substr(a, b - a);
}

// Shortest decimal that gives the units back: 0.2, 8, 0.35
void PutValue(std::string& Out, const char* pKey, unsigned nUnits, float nScale)
{
	char Buffer[48];
	int nDecimals = 0;
	while (nDecimals < 4 && roundf(roundf(nUnits / nScale * powf(10.0f, nDecimals)) /
	                                   powf(10.0f, nDecimals) * nScale) != nUnits)
		++nDecimals;
	snprintf(Buffer, sizeof(Buffer), "%s = %.*f\n", pKey, nDecimals, nUnits / nScale);
	Out += Buffer;
}
} // namespace

uint16_t Units(float nValue, float nScale, unsigned nMin, unsigned nMax)
{
	const float u = roundf(nValue * nScale);
	if (!(u >= static_cast<float>(nMin)))
		return static_cast<uint16_t>(nMin);
	if (u > static_cast<float>(nMax))
		return static_cast<uint16_t>(nMax);
	return static_cast<uint16_t>(u);
}

uint8_t Read(const FXSettings& FX, uint32_t p, uint8_t& v)
{
	switch (p)
	{
		case PReverb: v = FX.Reverb ? 1 : 0; return param::OK;
		case PChorus: v = FX.Chorus ? 1 : 0; return param::OK;
		case PVoices: v = FX.Voices; return param::OK;
		case PChorusType: v = FX.ChorusType; return param::OK;
		default: break;
	}
	const Wide* w = FindWide(p);
	if (!w)
		return param::BadAddress;
	const unsigned n = FX.*(w->Field);
	v = static_cast<uint8_t>((p == w->First ? n : n >> 7) & 0x7F);
	return param::OK;
}

uint8_t Write(FXSettings& FX, uint32_t nParameter, const uint8_t* pData, size_t nSize, size_t& nBad)
{
	FXSettings New = FX;
	for (size_t i = 0; i < nSize; ++i)
	{
		const uint32_t p = nParameter + static_cast<uint32_t>(i);
		const uint8_t b  = pData[i];
		nBad = i;
		switch (p)
		{
			case PReverb:
			case PChorus:
				if (b > 1)
					return param::BadValue;
				(p == PReverb ? New.Reverb : New.Chorus) = b != 0;
				continue;
			case PVoices:
				if (b > 99)
					return param::BadValue;
				New.Voices = b;
				continue;
			case PChorusType:
				if (b > 1)
					return param::BadValue;
				New.ChorusType = b;
				continue;
			default: break;
		}
		const Wide* w = FindWide(p);
		if (!w)
			return param::BadAddress;
		uint16_t& f = New.*(w->Field);
		f = p == w->First ? static_cast<uint16_t>((f & ~0x7Fu) | b) : static_cast<uint16_t>((f & 0x7Fu) | (b << 7));
	}

	// The 14-bit values once both halves are in: refused at the byte that
	// completed them (or at the first one written)
	for (const Wide& w : Wides)
	{
		const unsigned n = New.*(w.Field);
		if (n >= w.Min && n <= w.Max)
			continue;
		const uint32_t nEnd = nParameter + static_cast<uint32_t>(nSize);
		if (w.First + 1 >= nParameter && w.First + 1 < nEnd)
			nBad = w.First + 1 - nParameter;
		else if (w.First >= nParameter && w.First < nEnd)
			nBad = w.First - nParameter;
		else
			nBad = 0;
		return param::BadValue;
	}
	nBad = 0;
	FX = New;
	return param::OK;
}

std::string ToConfig(const FXSettings& FX)
{
	std::string Out = "# Golem effects profile (mt32-pi format)\n";
	PutValue(Out, "gain", FX.Gain, 1000.0f);
	Out += FX.Reverb ? "reverb = on\n" : "reverb = off\n";
	PutValue(Out, "reverb_damping", FX.Damping, 1000.0f);
	PutValue(Out, "reverb_level", FX.Level, 1000.0f);
	PutValue(Out, "reverb_room_size", FX.RoomSize, 1000.0f);
	PutValue(Out, "reverb_width", FX.Width, 100.0f);
	Out += FX.Chorus ? "chorus = on\n" : "chorus = off\n";
	PutValue(Out, "chorus_depth", FX.Depth, 10.0f);
	PutValue(Out, "chorus_level", FX.ChorusLevel, 1000.0f);
	PutValue(Out, "chorus_voices", FX.Voices, 1.0f);
	PutValue(Out, "chorus_speed", FX.Speed, 1000.0f);
	Out += FX.ChorusType ? "chorus_type = triangle\n" : "chorus_type = sine\n";
	PutValue(Out, "polyphony", FX.Polyphony, 1.0f);
	return Out;
}

bool FromConfig(const char* pText, size_t nSize, FXSettings& FX, size_t& nBadLine)
{
	FXSettings New = FX;
	size_t nLine = 0, i = 0;
	while (i < nSize)
	{
		size_t j = i;
		while (j < nSize && pText[j] != '\n')
			++j;
		std::string Line(pText + i, j - i);
		i = j + 1;
		++nLine;
		nBadLine = nLine;

		const size_t nComment = Line.find_first_of(";#");
		if (nComment != std::string::npos)
			Line.erase(nComment);
		Trim(Line);
		if (Line.empty() || Line[0] == '[') // sections are ignored, as in mt32-pi
			continue;
		const size_t nEquals = Line.find('=');
		if (nEquals == std::string::npos)
			return false;
		std::string Key = Line.substr(0, nEquals), Value = Line.substr(nEquals + 1);
		Trim(Key);
		Trim(Value);
		for (char& c : Key)
			c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

		bool bOK = false;
		if (Key == "reverb")
			bOK = ParseBool(Value.c_str(), New.Reverb);
		else if (Key == "chorus")
			bOK = ParseBool(Value.c_str(), New.Chorus);
		else if (Key == "chorus_voices")
		{
			uint16_t v = 0;
			bOK = ParseUnits(Value.c_str(), 1.0f, 0, 99, v);
			New.Voices = static_cast<uint8_t>(v);
		}
		else if (Key == "chorus_type")
		{
			bOK = true;
			if (!strcasecmp(Value.c_str(), "sine") || Value == "0")
				New.ChorusType = 0;
			else if (!strcasecmp(Value.c_str(), "triangle") || Value == "1")
				New.ChorusType = 1;
			else
				bOK = false;
		}
		else
			for (const Wide& w : Wides)
				if (Key == w.Key)
					bOK = ParseUnits(Value.c_str(), w.Scale, w.Min, w.Max, New.*(w.Field));
		if (!bOK)
			return false;
	}
	nBadLine = 0;
	FX = New;
	return true;
}

} // namespace fxparams
} // namespace golem
