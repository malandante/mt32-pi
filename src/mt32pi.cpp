//
// mt32pi.cpp
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

#include <circle/memory.h>
#include <circle/serial.h>
#include <circle/sound/hdmisoundbasedevice.h>
#include <circle/sound/i2ssoundbasedevice.h>
#include <circle/sound/pwmsoundbasedevice.h>

#include <cstdarg>
#include <cstdio>

#include "lcd/drivers/hd44780.h"
#include "lcd/drivers/ssd1306.h"
#include "lcd/ui.h"
#include "mt32pi.h"
#include "diagaudio.h"

#define MT32_PI_NAME "mt32-pi"
LOGMODULE(MT32_PI_NAME);
const char MT32PiFullName[] = MT32_PI_NAME " " MT32_PI_VERSION;

const char WLANFirmwarePath[] = "SD:firmware/";
const char WLANConfigFile[]   = "SD:wpa_supplicant.conf";

constexpr u32 LCDUpdatePeriodMillis                = 16;
constexpr u32 MisterUpdatePeriodMillis             = 50;
constexpr u32 LEDTimeoutMillis                     = 50;
constexpr u32 ActiveSenseTimeoutMillis             = 330;

constexpr float Sample24BitMax = (1 << 24 - 1) - 1;

enum class TCustomSysExCommand : u8
{
	Reboot                = 0x00,
	SwitchMT32ROMSet      = 0x01,
	SwitchSoundFont       = 0x02,
	SwitchSynth           = 0x03,
	SetMT32ReversedStereo = 0x04,
};

// Experimental Golem control protocol. 0x7D is retained while the protocol is
// under development; a production manufacturer ID must be selected before the
// wire format is declared stable.
constexpr u8 GolemSignature[]      = {0x47, 0x4C, 0x4D}; // "GLM"
constexpr u8 GolemProtocolVersion = 0x01;
constexpr size_t GolemHeaderSize  = 8;
constexpr size_t GolemMaxPayload  = 8;

enum class TGolemCommand : u8
{
	GetStatus      = 0x00,
	SetSynth       = 0x01,
	SetMT32ROMSet  = 0x02,
	SetSoundFont   = 0x03,
	SetUSBOut      = 0x04,
	SetParameter   = 0x05,
	Save           = 0x06,
};

enum class TGolemResponse : u8
{
	Accepted = 0x40,
	Ready    = 0x41,
	Error    = 0x42,
	Status   = 0x43,
};

enum class TGolemError : u8
{
	MalformedRequest   = 0x01,
	UnsupportedVersion = 0x02,
	UnknownCommand     = 0x03,
	InvalidParameter   = 0x04,
	Unavailable        = 0x05,
	OperationFailed    = 0x06,
};

enum TGolemCapability : u16
{
	GolemCapabilityStatus       = 1 << 0,
	GolemCapabilitySetSynth     = 1 << 1,
	GolemCapabilitySetROMSet    = 1 << 2,
	GolemCapabilitySetSoundFont = 1 << 3,
	GolemCapabilitySetUSBOut    = 1 << 4,
	GolemCapabilitySetParameter = 1 << 5,
	GolemCapabilitySave         = 1 << 6,
};

// SAVE payload
constexpr u8 GolemSaveWrite = 0x00;	// write golem.cfg with the current settings
constexpr u8 GolemSaveClear = 0x01;	// delete it: mt32-pi.cfg alone at the next start

constexpr char GolemConfigPath[]     = "SD:golem.cfg";
constexpr char GolemConfigTempPath[] = "SD:golem.tmp";

// SET_PARAMETER: parameter numbers and the value that only reports
enum class TGolemParameter : u8
{
	Volume       = 0x00,	// 0-100, both synths (as the buttons and encoder)
	Effects      = 0x01,	// 0/1, FluidSynth reverb and chorus
	MIDIChannels = 0x02,	// 0 standard (2-10), 1 alternate (1-8, 10), MT-32
	USBInThru    = 0x03,	// 0/1, USB MIDI input also to USB MIDI devices
	KeysToNext   = 0x04,	// 0/1, USB MIDI input also to the Next (GPIO serial)
	OPL3Bank     = 0x05,	// 0-127, OPL3 bank: embedded, then SD:opl3/*.wopl
	Loudness     = 0x06,	// 0/1, synth level raised with a limiter
};
constexpr u8 GolemParameterQuery = 0x7F;

// SET_USB_OUT parameter that only reports the current mode
constexpr u8 GolemUSBOutQuery = 0x7F;

CMT32Pi* CMT32Pi::s_pThis = nullptr;

CMT32Pi::CMT32Pi(CI2CMaster* pI2CMaster, CSPIMaster* pSPIMaster, CInterruptSystem* pInterrupt, CGPIOManager* pGPIOManager, CMIDISerialDevice* pSerialDevice, CUSBHCIDevice* pUSBHCI)
	: CMultiCoreSupport(CMemorySystem::Get()),
	  CMIDIParser(),

	  m_pConfig(CConfig::Get()),

	  m_pTimer(CTimer::Get()),
	  m_pActLED(CActLED::Get()),

	  m_pI2CMaster(pI2CMaster),
	  m_pSPIMaster(pSPIMaster),
	  m_pInterrupt(pInterrupt),
	  m_pGPIOManager(pGPIOManager),
	  m_pSerial(pSerialDevice),
	  m_pUSBHCI(pUSBHCI),
	  m_USBFileSystem{},
	  m_bUSBAvailable(false),

	  m_pNet(nullptr),
	  m_pNetDevice(nullptr),
	  m_WLAN(WLANFirmwarePath),
	  m_WPASupplicant(WLANConfigFile),
	  m_bNetworkReady(false),
	  m_pAppleMIDIParticipant(nullptr),
	  m_pUDPMIDIReceiver(nullptr),
	  m_pFTPDaemon(nullptr),

	  m_pLCD(nullptr),
	  m_nLCDUpdateTime(0),
#ifdef MONITOR_TEMPERATURE
	  m_nTempUpdateTime(0),
#endif

	  m_pControl(nullptr),
	  m_MisterControl(pI2CMaster, m_EventQueue),
	  m_nMisterUpdateTime(0),

	  m_bDeferredSoundFontSwitchFlag(false),
	  m_nDeferredSoundFontSwitchIndex(0),
	  m_nDeferredSoundFontSwitchTime(0),

	  m_bSerialMIDIAvailable(false),
	  m_bSerialMIDIEnabled(false),
	  m_pUSBMIDIDevices{},
	  m_pUSBSerialDevice(nullptr),
	  m_pUSBMassStorageDevice(nullptr),

	  m_bActiveSenseFlag(false),
	  m_nActiveSenseTime(0),

	  m_bRunning(true),
	  m_bUITaskDone(false),
	  m_bLEDOn(false),
	  m_nLEDOnTime(0),

	  m_pSound(nullptr),

	  m_pNextAudio(nullptr),
	  m_bLoudness(false),
	  m_pPisound(nullptr),

	  m_nMasterVolume(100),
	  m_pCurrentSynth(nullptr),
	  m_pMT32Synth(nullptr),
	  m_pSoundFontSynth(nullptr),
	  m_pOPL3Synth(nullptr),

	  m_ExternalMIDIParser(this),
	  m_USBSerialMIDIParser(this),
	  m_USBOutMode(CConfig::TMIDIUSBOut::Off),
	  m_nKeysToNextDropped(0),
	  m_nUSBTxDroppedReported(0)
{
	s_pThis = this;
}

CMT32Pi::~CMT32Pi()
{
}

bool CMT32Pi::Initialize(bool bSerialMIDIAvailable)
{
	m_bSerialMIDIAvailable = bSerialMIDIAvailable;
	m_bSerialMIDIEnabled = bSerialMIDIAvailable;
	m_USBOutMode = m_pConfig->MIDIUSBOut;

	switch (m_pConfig->LCDType)
	{
		case CConfig::TLCDType::HD44780FourBit:
			m_pLCD = new CHD44780FourBit(m_pConfig->LCDWidth, m_pConfig->LCDHeight);
			break;

		case CConfig::TLCDType::HD44780I2C:
			m_pLCD = new CHD44780I2C(m_pI2CMaster, m_pConfig->LCDI2CLCDAddress, m_pConfig->LCDWidth, m_pConfig->LCDHeight);
			break;

		case CConfig::TLCDType::SH1106I2C:
			m_pLCD = new CSH1106(m_pI2CMaster, m_pConfig->LCDI2CLCDAddress, m_pConfig->LCDWidth, m_pConfig->LCDHeight, m_pConfig->LCDRotation);
			break;

		case CConfig::TLCDType::SSD1306I2C:
			m_pLCD = new CSSD1306(m_pI2CMaster, m_pConfig->LCDI2CLCDAddress, m_pConfig->LCDWidth, m_pConfig->LCDHeight, m_pConfig->LCDRotation, m_pConfig->LCDMirror);
			break;

		default:
			break;
	}

	if (m_pLCD)
	{
		if (m_pLCD->Initialize())
		{
			CLogger::Get()->RegisterPanicHandler(PanicHandler);

			// Splash screen
			if (m_pLCD->GetType() == CLCD::TType::Graphical && !m_pConfig->SystemVerbose)
				m_pLCD->DrawImage(TImage::MT32PiLogo, true);
			else
			{
				const u8 nOffsetX = CUserInterface::CenterMessageOffset(*m_pLCD, MT32PiFullName);
				m_pLCD->Print(MT32PiFullName, nOffsetX, 0, false, true);
			}
		}
		else
		{
			LOGWARN("LCD init failed; invalid dimensions?");
			delete m_pLCD;
			m_pLCD = nullptr;
		}
	}

#if !defined(__aarch64__) || !defined(LEAVE_QEMU_ON_HALT)
	// The USB driver is not supported under 64-bit QEMU, so
	// the initialization must be skipped in this case, or an
	// exit happens here under 64-bit QEMU.
	LCDLog(TLCDLogType::Startup, "Init USB");
	if (m_pConfig->SystemUSB && m_pUSBHCI->Initialize())
	{
		m_bUSBAvailable = true;

		// Perform an initial Plug and Play update to initialize devices early
		UpdateUSB(true);
	}
#endif

	LCDLog(TLCDLogType::Startup, "Init Network");
	InitNetwork();

	// Check for Blokas Pisound, but only when not using 4-bit HD44780 (GPIO pin conflict)
	if (m_pConfig->LCDType != CConfig::TLCDType::HD44780FourBit)
	{
		m_pPisound = new CPisound(m_pSPIMaster, m_pGPIOManager, m_pConfig->AudioSampleRate);
		if (m_pPisound->Initialize())
		{
			LOGWARN("Blokas Pisound detected");
			m_pPisound->RegisterMIDIReceiveHandler(IRQMIDIReceiveHandler);
			m_bSerialMIDIEnabled = false;
		}
		else
		{
			delete m_pPisound;
			m_pPisound = nullptr;
		}
	}

	// Queue size of just one chunk
	unsigned int nQueueSize = m_pConfig->AudioChunkSize;
	TSoundFormat Format = TSoundFormat::SoundFormatSigned24;

	switch (m_pConfig->AudioOutputDevice)
	{
		case CConfig::TAudioOutputDevice::PWM:
			LCDLog(TLCDLogType::Startup, "Init audio (PWM)");
			m_pSound = new CPWMSoundBaseDevice(m_pInterrupt, m_pConfig->AudioSampleRate, m_pConfig->AudioChunkSize);
			break;

		case CConfig::TAudioOutputDevice::HDMI:
		{
			LCDLog(TLCDLogType::Startup, "Init audio (HDMI)");

			// Chunk size must be a multiple of 384
			const unsigned int nChunkSize = Utility::RoundToNearestMultiple(m_pConfig->AudioChunkSize, IEC958_SUBFRAMES_PER_BLOCK);
			nQueueSize = nChunkSize;

			m_pSound = new CHDMISoundBaseDevice(m_pInterrupt, m_pConfig->AudioSampleRate, nChunkSize);
			break;
		}

		case CConfig::TAudioOutputDevice::I2S:
		{
			LCDLog(TLCDLogType::Startup, "Init audio (I2S)");

			// Pisound provides clock
			const bool bSlave = m_pPisound != nullptr;

			// Don't probe if using Pisound
			CI2CMaster* const pI2CMaster = bSlave ? nullptr : m_pI2CMaster;

			m_pSound = new CI2SSoundBaseDevice(m_pInterrupt, m_pConfig->AudioSampleRate, m_pConfig->AudioChunkSize, bSlave, pI2CMaster);
			Format = TSoundFormat::SoundFormatSigned24_32;

			break;
		}
	}

	m_pSound->SetWriteFormat(Format);
	if (!m_pSound->AllocateQueueFrames(nQueueSize))
		LOGPANIC("Failed to allocate sound queue");

	// Experimental: the Next's own audio over I2S, mixed into a PWM or HDMI
	// output (with I2S output it would go back into the Next)
	if (m_pConfig->AudioNextInput)
	{
		if (m_pConfig->AudioOutputDevice == CConfig::TAudioOutputDevice::I2S || m_pPisound)
			LOGWARN("next_input needs output_device = pwm or hdmi; ignored");
		else
		{
			LCDLog(TLCDLogType::Startup, "Init Next audio in");
			m_pNextAudio = new CNextAudioInput(m_pInterrupt, m_pConfig->AudioSampleRate, m_pConfig->AudioChunkSize);
		}
	}

	LCDLog(TLCDLogType::Startup, "Init controls");
	if (m_pConfig->ControlScheme == CConfig::TControlScheme::SimpleButtons)
		m_pControl = new CControlSimpleButtons(m_EventQueue);
	else if (m_pConfig->ControlScheme == CConfig::TControlScheme::SimpleEncoder)
		m_pControl = new CControlSimpleEncoder(m_EventQueue, m_pConfig->ControlEncoderType, m_pConfig->ControlEncoderReversed);

	if (m_pControl && !m_pControl->Initialize())
	{
		LOGWARN("Control init failed");
		delete m_pControl;
		m_pControl = nullptr;
	}

	LCDLog(TLCDLogType::Startup, "Init mt32emu");
	InitMT32Synth();

	LCDLog(TLCDLogType::Startup, "Init FluidSynth");
	InitSoundFontSynth();

	LCDLog(TLCDLogType::Startup, "Init OPL3");
	InitOPL3Synth();

	// Set initial synthesizer
	if (m_pConfig->SystemDefaultSynth == CConfig::TSystemDefaultSynth::MT32)
		m_pCurrentSynth = m_pMT32Synth;
	else if (m_pConfig->SystemDefaultSynth == CConfig::TSystemDefaultSynth::SoundFont)
		m_pCurrentSynth = m_pSoundFontSynth;
	else if (m_pConfig->SystemDefaultSynth == CConfig::TSystemDefaultSynth::OPL3)
		m_pCurrentSynth = m_pOPL3Synth;

	if (!m_pCurrentSynth)
	{
		LOGERR("Preferred synth failed to initialize successfully");

		// Activate any working synth
		if (m_pMT32Synth)
			m_pCurrentSynth = m_pMT32Synth;
		else if (m_pSoundFontSynth)
			m_pCurrentSynth = m_pSoundFontSynth;
		else if (m_pOPL3Synth)
			m_pCurrentSynth = m_pOPL3Synth;
		else
		{
			LOGPANIC("No synths available; ROMs/SoundFonts not found");
			return false;
		}
	}

	// [golem] settings from mt32-pi.cfg or golem.cfg (.golem save)
	if (m_pConfig->GolemVolume != 100)
		SetMasterVolume(m_pConfig->GolemVolume);
	if (m_pConfig->GolemEffects >= 0 && m_pSoundFontSynth)
		m_pSoundFontSynth->SetEffects(m_pConfig->GolemEffects != 0);
	m_bLoudness = m_pConfig->GolemLoudness;

	if (m_pPisound)
		LOGNOTE("Using Pisound MIDI interface");
	else if (m_bSerialMIDIEnabled)
		LOGNOTE("Using serial MIDI interface");

	CCPUThrottle::Get()->DumpStatus();

	// The Next may be playing even when the synth is idle: no power saving
	SetPowerSaveTimeout(m_pNextAudio ? 0 : m_pConfig->SystemPowerSaveTimeout);

	// Clear LCD
	if (m_pLCD)
		m_pLCD->Clear();

	// Start audio
	m_pSound->Start();
	if (m_pNextAudio && !m_pNextAudio->Start())
	{
		LOGERR("Next audio input failed to start");
		delete m_pNextAudio;
		m_pNextAudio = nullptr;
	}

	// Start other cores
	if (!CMultiCoreSupport::Initialize())
		return false;

	return true;
}

bool CMT32Pi::InitNetwork()
{
	assert(m_pNet == nullptr);

	TNetDeviceType NetDeviceType = NetDeviceTypeUnknown;

	if (m_pConfig->NetworkMode == CConfig::TNetworkMode::WiFi)
	{
		LOGNOTE("Initializing Wi-Fi");

		if (m_WLAN.Initialize() && m_WPASupplicant.Initialize())
			NetDeviceType = NetDeviceTypeWLAN;
		else
			LOGERR("Failed to initialize Wi-Fi");
	}
	else if (m_pConfig->NetworkMode == CConfig::TNetworkMode::Ethernet)
	{
		LOGNOTE("Initializing Ethernet");
		NetDeviceType = NetDeviceTypeEthernet;
	}

	if (NetDeviceType != NetDeviceTypeUnknown)
	{
		if (m_pConfig->NetworkDHCP)
			m_pNet = new CNetSubSystem(0, 0, 0, 0, m_pConfig->NetworkHostname, NetDeviceType);
		else
			m_pNet = new CNetSubSystem(
				m_pConfig->NetworkIPAddress.Get(),
				m_pConfig->NetworkSubnetMask.Get(),
				m_pConfig->NetworkDefaultGateway.Get(),
				m_pConfig->NetworkDNSServer.Get(),
				m_pConfig->NetworkHostname,
				NetDeviceType
			);

		if (!m_pNet->Initialize(false))
		{
			LOGERR("Failed to initialize network subsystem");
			delete m_pNet;
			m_pNet = nullptr;
		}

		m_pNetDevice = CNetDevice::GetNetDevice(NetDeviceType);
	}

	return m_pNet != nullptr;
}

bool CMT32Pi::InitMT32Synth()
{
	assert(m_pMT32Synth == nullptr);

	m_pMT32Synth = new CMT32Synth(m_pConfig->AudioSampleRate, m_pConfig->MT32EmuGain, m_pConfig->MT32EmuReverbGain, m_pConfig->MT32EmuResamplerQuality);
	if (!m_pMT32Synth->Initialize())
	{
		LOGWARN("mt32emu init failed; no ROMs present?");
		delete m_pMT32Synth;
		m_pMT32Synth = nullptr;
		return false;
	}

	// Set initial MT-32 channel assignment from config
	if (m_pConfig->MT32EmuMIDIChannels == CMT32Synth::TMIDIChannels::Alternate)
		m_pMT32Synth->SetMIDIChannels(m_pConfig->MT32EmuMIDIChannels);

	// Set MT-32 reversed stereo option from config
	m_pMT32Synth->SetReversedStereo(m_pConfig->MT32EmuReversedStereo);

	m_pMT32Synth->SetUserInterface(&m_UserInterface);

	return true;
}

bool CMT32Pi::InitSoundFontSynth()
{
	assert(m_pSoundFontSynth == nullptr);

	m_pSoundFontSynth = new CSoundFontSynth(m_pConfig->AudioSampleRate);
	if (!m_pSoundFontSynth->Initialize())
	{
		LOGWARN("FluidSynth init failed; no SoundFonts present?");
		delete m_pSoundFontSynth;
		m_pSoundFontSynth = nullptr;
		return false;
	}

	m_pSoundFontSynth->SetUserInterface(&m_UserInterface);

	return true;
}

bool CMT32Pi::InitOPL3Synth()
{
	assert(m_pOPL3Synth == nullptr);

	// Needs no files: the instrument banks are built in
	m_pOPL3Synth = new COPL3Synth(m_pConfig->AudioSampleRate);
	if (!m_pOPL3Synth->Initialize())
	{
		LOGWARN("OPL3 init failed");
		delete m_pOPL3Synth;
		m_pOPL3Synth = nullptr;
		return false;
	}

	m_pOPL3Synth->SetUserInterface(&m_UserInterface);

	return true;
}

void CMT32Pi::MainTask()
{
	CScheduler* const pScheduler = CScheduler::Get();

	LOGNOTE("Main task on Core 0 starting up");

	Awaken();

	while (m_bRunning)
	{
		// Process MIDI data
		UpdateMIDI();

		// Send forwarded MIDI to USB devices
		FlushUSBMIDI();

		// Process network packets
		UpdateNetwork();

		// Update controls
		if (m_pControl)
			m_pControl->Update();

		// Process events
		ProcessEventQueue();

		const unsigned int nTicks = m_pTimer->GetTicks();

		// Update activity LED
		if (m_bLEDOn && (nTicks - m_nLEDOnTime) >= MSEC2HZ(LEDTimeoutMillis))
		{
			m_pActLED->Off();
			m_bLEDOn = false;
		}

		// Check for active sensing timeout
		if (m_bActiveSenseFlag && (nTicks > m_nActiveSenseTime) && (nTicks - m_nActiveSenseTime) >= MSEC2HZ(ActiveSenseTimeoutMillis))
		{
			m_pCurrentSynth->AllSoundOff();
			m_bActiveSenseFlag = false;
			LOGNOTE("Active sense timeout - turning notes off");
		}

		// Update power management
		if (m_pCurrentSynth->IsActive())
			Awaken();

#ifdef MONITOR_TEMPERATURE
		if (nTicks - m_nTempUpdateTime >= MSEC2HZ(5000))
		{
			const unsigned int nTemp = CCPUThrottle::Get()->GetTemperature();
			LOGDBG("Temperature: %dC", nTemp);
			LCDLog(TLCDLogType::Notice, "Temp: %dC", nTemp);
			m_nTempUpdateTime = nTicks;
		}
#endif

		CPower::Update();

		// Check for deferred SoundFont switch
		if (m_bDeferredSoundFontSwitchFlag)
		{
			// Delay switch if scrolling a long SoundFont name
			if (m_UserInterface.IsScrolling())
				m_nDeferredSoundFontSwitchTime = nTicks;
			else if ((nTicks - m_nDeferredSoundFontSwitchTime) >= static_cast<unsigned int>(m_pConfig->ControlSwitchTimeout) * HZ)
			{
				SwitchSoundFont(m_nDeferredSoundFontSwitchIndex);
				m_bDeferredSoundFontSwitchFlag = false;

				// Trigger an awaken so we don't immediately go to sleep
				Awaken();
			}
		}

		// Check for USB PnP events
		UpdateUSB();

		// Allow other tasks to run
		pScheduler->Yield();
	}


	// Stop audio
	m_pSound->Cancel();

	// Wait for UI task to finish
	while (!m_bUITaskDone)
		;
}

void CMT32Pi::UITask()
{
	LOGNOTE("UI task on Core 1 starting up");

	const bool bMisterEnabled = m_pConfig->ControlMister;

	// Nothing for this core to do; bail out
	if (!(m_pLCD || bMisterEnabled))
	{
		m_bUITaskDone = true;
		return;
	}

	// Display current MT-32 ROM version/SoundFont
	m_pCurrentSynth->ReportStatus();

	unsigned int nNextAudioLogTime = CTimer::GetClockTicks();

	while (m_bRunning)
	{
		const unsigned int nTicks = CTimer::GetClockTicks();

		// Update LCD
		if (m_pLCD && (nTicks - m_nLCDUpdateTime) >= Utility::MillisToTicks(LCDUpdatePeriodMillis))
		{
			m_UserInterface.Update(*m_pLCD, *m_pCurrentSynth, nTicks);
			m_nLCDUpdateTime = nTicks;
		}

		// Experimental Next audio input: its counters every 10 seconds
		if (m_pNextAudio && (nTicks - nNextAudioLogTime) >= Utility::MillisToTicks(10000u))
		{
			m_pNextAudio->LogStatus();
			nNextAudioLogTime = nTicks;
		}

		// Poll MiSTer interface
		if (bMisterEnabled && (nTicks - m_nMisterUpdateTime) >= Utility::MillisToTicks(MisterUpdatePeriodMillis))
		{
			TMisterStatus Status{TMisterSynth::Unknown, 0xFF, 0xFF};

			if (m_pCurrentSynth == m_pMT32Synth)
				Status.Synth = TMisterSynth::MT32;
			else if (m_pCurrentSynth == m_pSoundFontSynth)
				Status.Synth = TMisterSynth::SoundFont;

			if (m_pMT32Synth)
				Status.MT32ROMSet = static_cast<u8>(m_pMT32Synth->GetROMSet());

			if (m_pSoundFontSynth)
				Status.SoundFontIndex = m_pSoundFontSynth->GetSoundFontIndex();

			m_MisterControl.Update(Status);
			m_nMisterUpdateTime = nTicks;
		}
	}

	// Clear screen
	if (m_pLCD)
		m_pLCD->Clear();

	m_bUITaskDone = true;
}

void CMT32Pi::AudioTask()
{
	LOGNOTE("Audio task on Core 2 starting up");

	constexpr u8 nChannels = 2;

	// Circle's "fast path" for I2S 24-bit really expects 32-bit samples
	const bool bI2S = m_pConfig->AudioOutputDevice == CConfig::TAudioOutputDevice::I2S;
	const bool bReversedStereo = m_pConfig->AudioReversedStereo;
	const u8 nBytesPerSample = bI2S ? sizeof(s32) : (sizeof(s8) * 3);
	const u8 nBytesPerFrame = 2 * nBytesPerSample;

	const size_t nQueueSizeFrames = m_pSound->GetQueueSizeFrames();
	const float nNextInputGain = m_pConfig->AudioNextInputGain;
	bool bLimiting = false;

	// Extra byte so that we can write to the 24-bit buffer with overlapping 32-bit writes (efficiency)
	float FloatBuffer[nQueueSizeFrames * nChannels];
	// (was 'n + bI2S ? 0 : 1', which parses as '(n + bI2S) ? 0 : 1': a zero-length
	// array, so the conversion wrote over FloatBuffer on the stack)
	s8 IntBuffer[nQueueSizeFrames * nBytesPerFrame + (bI2S ? 0 : 1)];

	while (m_bRunning)
	{
		const size_t nFrames = nQueueSizeFrames - m_pSound->GetQueueFramesAvail();
		const size_t nWriteBytes = nFrames * nBytesPerFrame;

		m_pCurrentSynth->Render(FloatBuffer, nFrames);
		DiagAudio::Measure(FloatBuffer, nFrames, false, m_pConfig->AudioSampleRate);

		// Loudness: a fresh limiter each time it is switched on, so no old
		// audio is left in its look-ahead
		if (m_bLoudness)
		{
			if (!bLimiting)
			{
				m_Limiter = Loudness::CLimiter();
				m_Limiter.Configure(m_pConfig->AudioSampleRate, m_pConfig->GolemLoudnessGain);
				bLimiting = true;
			}
			m_Limiter.Process(FloatBuffer, nFrames);
		}
		else
			bLimiting = false;

		if (m_pNextAudio)
			m_pNextAudio->MixInto(FloatBuffer, nFrames, nNextInputGain);
		DiagAudio::Measure(FloatBuffer, nFrames, true, m_pConfig->AudioSampleRate);

		if (bReversedStereo)
		{
			// Convert to signed 24-bit integers with channel swap
			for (size_t i = 0; i < nFrames * nChannels; i += nChannels)
			{
				s32* const pLeftSample = reinterpret_cast<s32*>(IntBuffer + i * nBytesPerSample);
				s32* const pRightSample = reinterpret_cast<s32*>(IntBuffer + (i + 1) * nBytesPerSample);
				*pLeftSample = FloatBuffer[i + 1] * Sample24BitMax;
				*pRightSample = FloatBuffer[i] * Sample24BitMax;
			}
		}
		else
		{
			// Convert to signed 24-bit integers
			for (size_t i = 0; i < nFrames * nChannels; ++i)
			{
				s32* const pSample = reinterpret_cast<s32*>(IntBuffer + i * nBytesPerSample);
				*pSample = FloatBuffer[i] * Sample24BitMax;
			}
		}

		const int nResult = m_pSound->Write(IntBuffer, nWriteBytes);
		if (nResult != static_cast<int>(nWriteBytes))
			LOGERR("Sound data dropped");
	}
}

void CMT32Pi::Run(unsigned nCore)
{
	// Assign tasks to different CPU cores
	switch (nCore)
	{
		case 0:
			return MainTask();

		case 1:
			return UITask();

		case 2:
			return AudioTask();

		default:
			break;
	}
}

void CMT32Pi::OnEnterPowerSavingMode()
{
	CPower::OnEnterPowerSavingMode();
	m_pSound->Cancel();
	m_UserInterface.EnterPowerSavingMode();
}

void CMT32Pi::OnExitPowerSavingMode()
{
	CPower::OnExitPowerSavingMode();
	m_pSound->Start();
	m_UserInterface.ExitPowerSavingMode();
}

void CMT32Pi::OnThrottleDetected()
{
	CPower::OnThrottleDetected();
	LCDLog(TLCDLogType::Warning, "CPU throttl! Chk PSU");
}

void CMT32Pi::OnUnderVoltageDetected()
{
	CPower::OnUnderVoltageDetected();
	LCDLog(TLCDLogType::Warning, "Low voltage! Chk PSU");
}

// MIDI from the host: GPIO serial (the Next) and network MIDI
void CMT32Pi::OnShortMessage(u32 nMessage)
{
	if (m_USBOutMode != CConfig::TMIDIUSBOut::Off)
		QueueUSBShortMessage(nMessage);

	// Bridge mode: the synth stays silent while the host plays a USB device
	if (m_USBOutMode == CConfig::TMIDIUSBOut::Bridge)
	{
		if ((nMessage & 0xFF) < 0xF0)
			LEDOn();
		return;
	}

	HandleShortMessage(nMessage);
}

void CMT32Pi::OnSysExMessage(const u8* pData, size_t nSize)
{
	// Flash LED
	LEDOn();

	// mt32-pi and Golem control messages are consumed here and never forwarded
	if (!ParseCustomSysEx(pData, nSize))
	{
		if (m_USBOutMode != CConfig::TMIDIUSBOut::Off)
			QueueUSBSysEx(pData, nSize);

		if (m_USBOutMode != CConfig::TMIDIUSBOut::Bridge)
			m_pCurrentSynth->HandleMIDISysExMessage(pData, nSize);
	}

	// Wake from power saving mode if necessary
	Awaken();
}

// MIDI from USB devices, USB serial or the Pisound: always reaches the synth
void CMT32Pi::OnExternalShortMessage(u32 nMessage)
{
	if (m_pConfig->MIDIUSBInThru)
		QueueUSBShortMessage(nMessage);

	// Keyboard to the Next: channel messages and system common, whole; no
	// real-time bytes (clock, active sensing), which would only flood the port
	const u8 nStatus = nMessage & 0xFF;
	if (m_pConfig->GolemKeysToNext && nStatus < 0xF8)
	{
		const u8 Message[] = {nStatus, static_cast<u8>((nMessage >> 8) & 0x7F), static_cast<u8>((nMessage >> 16) & 0x7F)};
		size_t nLength = 3;
		if ((nStatus >= 0xC0 && nStatus < 0xE0) || nStatus == 0xF1 || nStatus == 0xF3)
			nLength = 2;
		else if (nStatus >= 0xF4)
			nLength = 1;
		SendToHost(Message, nLength);
	}

	HandleShortMessage(nMessage);
}

void CMT32Pi::OnExternalSysExMessage(const u8* pData, size_t nSize)
{
	// Golem replies always go to the GPIO serial port, so a Golem request
	// from another source would answer the wrong device: ignore it.
	if (nSize >= 5 && pData[1] == 0x7D &&
	    pData[2] == GolemSignature[0] &&
	    pData[3] == GolemSignature[1] &&
	    pData[4] == GolemSignature[2])
		return;

	LEDOn();

	if (!ParseCustomSysEx(pData, nSize))
	{
		if (m_pConfig->MIDIUSBInThru)
			QueueUSBSysEx(pData, nSize);

		if (m_pConfig->GolemKeysToNext)
			SendToHost(pData, nSize);

		m_pCurrentSynth->HandleMIDISysExMessage(pData, nSize);
	}

	Awaken();
}

void CMT32Pi::HandleShortMessage(u32 nMessage)
{
	// Active sensing
	if (nMessage == 0xFE)
	{
		m_bActiveSenseFlag = true;
		return;
	}

	// Flash LED for channel messages
	if ((nMessage & 0xFF) < 0xF0)
		LEDOn();

	m_pCurrentSynth->HandleMIDIShortMessage(nMessage);

	// Wake from power saving mode if necessary
	Awaken();
}

void CMT32Pi::QueueUSBShortMessage(u32 nMessage)
{
	u8 Packet[USBMIDI::PacketSize];
	if (USBMIDI::EncodeShortMessage(nMessage, 0, Packet))
		m_USBTxQueue.Push(Packet, sizeof(Packet));
}

void CMT32Pi::QueueUSBSysEx(const u8* pData, size_t nSize)
{
	// The parser's SysEx buffer is 1000 bytes, so this always fits
	u8 Packets[USBMIDI::SysExEncodedSize(1000)];
	if (nSize > 1000)
		return;

	const size_t nEncoded = USBMIDI::EncodeSysEx(pData, nSize, 0, Packets);
	if (nEncoded)
		m_USBTxQueue.Push(Packets, nEncoded);
}

// A whole message to the Next on the GPIO serial port, or nothing: a message
// cut by a full transmit buffer would corrupt the Next's MIDI stream and the
// Golem replies that share it. Runs in the main task, like the replies, so the
// two never interleave.
void CMT32Pi::SendToHost(const u8* pData, size_t nSize)
{
	if (!m_bSerialMIDIEnabled || m_pSerial == nullptr)
		return;

	if (m_pSerial->GetFreeWriteSpace() < nSize)
	{
		if (m_nKeysToNextDropped++ == 0)
			LOGWARN("Keys to Next: serial port busy, message dropped");
		return;
	}

	m_pSerial->Write(pData, nSize);
}

size_t CMT32Pi::GetUSBMIDIDeviceCount() const
{
	size_t nCount = 0;
	for (CUSBMIDIDevice* pDevice : m_pUSBMIDIDevices)
		if (pDevice)
			++nCount;
	return nCount;
}

// Send queued USB MIDI event packets to every attached USB MIDI device.
// Called from the main loop so the synchronous USB transfer never runs in the
// middle of parsing.
void CMT32Pi::FlushUSBMIDI()
{
	if (m_USBTxQueue.GetCount() == 0)
		return;

	if (GetUSBMIDIDeviceCount() == 0)
	{
		m_USBTxQueue.Clear();
		return;
	}

	// Bound the work per main loop iteration
	u8 Buffer[256];
	for (size_t nBatches = 0; nBatches < 4; ++nBatches)
	{
		const size_t nSize = m_USBTxQueue.Pop(Buffer, sizeof(Buffer));
		if (nSize == 0)
			break;

		for (CUSBMIDIDevice* pDevice : m_pUSBMIDIDevices)
			if (pDevice && !pDevice->SendEventPackets(Buffer, nSize) && m_pConfig->SystemVerbose)
				LOGWARN("USB MIDI send failed");
	}

	if (m_USBTxQueue.GetDropped() != m_nUSBTxDroppedReported)
	{
		m_nUSBTxDroppedReported = m_USBTxQueue.GetDropped();
		LOGWARN("USB MIDI output overflow");
		LCDLog(TLCDLogType::Error, "USB out overflow!");
	}
}

void CMT32Pi::SetUSBOutMode(CConfig::TMIDIUSBOut Mode)
{
	if (Mode == m_USBOutMode)
		return;

	const CConfig::TMIDIUSBOut OldMode = m_USBOutMode;
	m_USBOutMode = Mode;

	// Leaving forwarding: release notes held on the USB device
	// (CC 120 All Sound Off and CC 123 All Notes Off on every channel)
	if (Mode == CConfig::TMIDIUSBOut::Off)
	{
		for (u32 nChannel = 0; nChannel < 16; ++nChannel)
		{
			QueueUSBShortMessage(0xB0 | nChannel | 120 << 8);
			QueueUSBShortMessage(0xB0 | nChannel | 123 << 8);
		}
	}

	// Entering bridge mode: notes already sounding would never get a Note Off
	if (Mode == CConfig::TMIDIUSBOut::Bridge || OldMode == CConfig::TMIDIUSBOut::Bridge)
	{
		if (m_pMT32Synth)
			m_pMT32Synth->AllSoundOff();
		if (m_pSoundFontSynth)
			m_pSoundFontSynth->AllSoundOff();
		if (m_pOPL3Synth)
			m_pOPL3Synth->AllSoundOff();
	}

	const char* pModeName = Mode == CConfig::TMIDIUSBOut::Off    ? "off" :
	                        Mode == CConfig::TMIDIUSBOut::Thru   ? "on" :
	                                                               "bridge";
	LOGNOTE("USB MIDI out: %s", pModeName);
	LCDLog(TLCDLogType::Notice, "USB out: %s", pModeName);
}

void CMT32Pi::OnUnexpectedStatus()
{
	CMIDIParser::OnUnexpectedStatus();
	if (m_pConfig->SystemVerbose)
		LCDLog(TLCDLogType::Warning, "Unexp. MIDI status!");
}

void CMT32Pi::OnSysExOverflow()
{
	CMIDIParser::OnSysExOverflow();
	LCDLog(TLCDLogType::Error, "SysEx overflow!");
}

void CMT32Pi::OnAppleMIDIConnect(const CIPAddress* pIPAddress, const char* pName)
{
	if (!m_pLCD)
		return;

	LCDLog(TLCDLogType::Notice, "%s connected!", pName);
}

void CMT32Pi::OnAppleMIDIDisconnect(const CIPAddress* pIPAddress, const char* pName)
{
	if (!m_pLCD)
		return;

	LCDLog(TLCDLogType::Notice, "%s disconnected!", pName);
}

bool CMT32Pi::ParseCustomSysEx(const u8* pData, size_t nSize)
{
	if (nSize < 4)
		return false;

	// 'Educational' manufacturer
	if (pData[1] != 0x7D)
		return false;

	// Versioned Golem messages use a three-byte signature so they cannot
	// collide with the original five-byte mt32-pi commands.
	if (nSize >= 5 &&
	    pData[2] == GolemSignature[0] &&
	    pData[3] == GolemSignature[1] &&
	    pData[4] == GolemSignature[2])
		return ParseGolemSysEx(pData, nSize);

	const auto Command = static_cast<TCustomSysExCommand>(pData[2]);

	// Reboot (F0 7D 00 F7)
	if (nSize == 4 && Command == TCustomSysExCommand::Reboot)
	{
		LOGNOTE("Reboot command received");
		m_bRunning = false;
		return true;
	}

	if (nSize != 5)
		return false;

	const u8 nParameter = pData[3];
	switch (Command)
	{
		// Switch MT-32 ROM set (F0 7D 01 xx F7)
		case TCustomSysExCommand::SwitchMT32ROMSet:
		{
			const TMT32ROMSet NewROMSet = static_cast<TMT32ROMSet>(nParameter);
			if (NewROMSet < TMT32ROMSet::Any)
				SwitchMT32ROMSet(NewROMSet);
			return true;
		}

		// Switch SoundFont (F0 7D 02 xx F7)
		case TCustomSysExCommand::SwitchSoundFont:
			SwitchSoundFont(nParameter);
			return true;

		// Switch synthesizer (F0 7D 03 xx F7)
		case TCustomSysExCommand::SwitchSynth:
		{
			SwitchSynth(static_cast<TSynth>(nParameter));
			return true;
		}

		// Swap MT-32 stereo channels (F0 7D 04 xx F7)
		case TCustomSysExCommand::SetMT32ReversedStereo:
		{
			if (m_pMT32Synth)
				m_pMT32Synth->SetReversedStereo(nParameter);
			return true;
		}

		default:
			return false;
	}
}

bool CMT32Pi::ParseGolemSysEx(const u8* pData, size_t nSize)
{
	// Request: F0 7D 47 4C 4D vv tt cc [payload...] F7
	if (nSize < GolemHeaderSize + 1)
		return true;

	const u8 nVersion     = pData[5];
	const u8 nTransaction = pData[6];
	const u8 nCommand     = pData[7];
	const auto Command    = static_cast<TGolemCommand>(nCommand);

	if (pData[nSize - 1] != 0xF7)
	{
		SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::MalformedRequest));
		return true;
	}

	if (nVersion != GolemProtocolVersion)
	{
		SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::UnsupportedVersion));
		return true;
	}

	switch (Command)
	{
		case TGolemCommand::GetStatus:
			if (nSize != GolemHeaderSize + 1)
			{
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::MalformedRequest));
				return true;
			}

			SendGolemResponse(nTransaction, static_cast<u8>(TGolemResponse::Accepted), nCommand);
			SendGolemStatus(nTransaction);
			return true;

		case TGolemCommand::SetSynth:
		{
			if (nSize != GolemHeaderSize + 2)
			{
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::MalformedRequest));
				return true;
			}

			const u8 nSynth = pData[8];
			if (nSynth > static_cast<u8>(TSynth::OPL3))
			{
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::InvalidParameter));
				return true;
			}

			CSynthBase* pRequestedSynth = nullptr;
			if (nSynth == static_cast<u8>(TSynth::MT32))
				pRequestedSynth = m_pMT32Synth;
			else if (nSynth == static_cast<u8>(TSynth::OPL3))
				pRequestedSynth = m_pOPL3Synth;
			else
				pRequestedSynth = m_pSoundFontSynth;
			if (pRequestedSynth == nullptr)
			{
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::Unavailable));
				return true;
			}

			SendGolemResponse(nTransaction, static_cast<u8>(TGolemResponse::Accepted), nCommand);
			SwitchSynth(static_cast<TSynth>(nSynth));
			if (m_pCurrentSynth == pRequestedSynth)
				SendGolemResponse(nTransaction, static_cast<u8>(TGolemResponse::Ready), nCommand, &nSynth, 1);
			else
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::OperationFailed));
			return true;
		}

		case TGolemCommand::SetMT32ROMSet:
		{
			if (nSize != GolemHeaderSize + 2)
			{
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::MalformedRequest));
				return true;
			}

			const u8 nROMSet = pData[8];
			if (nROMSet >= static_cast<u8>(TMT32ROMSet::Any))
			{
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::InvalidParameter));
				return true;
			}

			const auto ROMSet = static_cast<TMT32ROMSet>(nROMSet);
			if (m_pMT32Synth == nullptr || !m_pMT32Synth->GetROMManager().HaveROMSet(ROMSet))
			{
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::Unavailable));
				return true;
			}

			SendGolemResponse(nTransaction, static_cast<u8>(TGolemResponse::Accepted), nCommand);
			SwitchMT32ROMSet(ROMSet);
			if (m_pMT32Synth->GetROMSet() == ROMSet)
				SendGolemResponse(nTransaction, static_cast<u8>(TGolemResponse::Ready), nCommand, &nROMSet, 1);
			else
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::OperationFailed));
			return true;
		}

		case TGolemCommand::SetSoundFont:
		{
			if (nSize != GolemHeaderSize + 3)
			{
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::MalformedRequest));
				return true;
			}

			const size_t nSoundFont = pData[8] | (static_cast<size_t>(pData[9]) << 7);
			if (m_pSoundFontSynth == nullptr ||
			    nSoundFont >= m_pSoundFontSynth->GetSoundFontManager().GetSoundFontCount())
			{
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::Unavailable));
				return true;
			}

			const u8 Payload[] = {static_cast<u8>(nSoundFont & 0x7F), static_cast<u8>((nSoundFont >> 7) & 0x7F)};
			SendGolemResponse(nTransaction, static_cast<u8>(TGolemResponse::Accepted), nCommand);
			SwitchSoundFont(nSoundFont);
			if (m_pSoundFontSynth->GetSoundFontIndex() == nSoundFont)
				SendGolemResponse(nTransaction, static_cast<u8>(TGolemResponse::Ready), nCommand, Payload, sizeof(Payload));
			else
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::OperationFailed));
			return true;
		}

		case TGolemCommand::SetUSBOut:
		{
			// Payload: 0 = off, 1 = on (synth also plays), 2 = bridge (synth
			// silent), 0x7F = report only. READY carries the mode and the
			// number of attached USB MIDI devices.
			if (nSize != GolemHeaderSize + 2)
			{
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::MalformedRequest));
				return true;
			}

			const u8 nMode = pData[8];
			if (nMode > static_cast<u8>(CConfig::TMIDIUSBOut::Bridge) && nMode != GolemUSBOutQuery)
			{
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::InvalidParameter));
				return true;
			}

			SendGolemResponse(nTransaction, static_cast<u8>(TGolemResponse::Accepted), nCommand);
			if (nMode != GolemUSBOutQuery)
				SetUSBOutMode(static_cast<CConfig::TMIDIUSBOut>(nMode));

			const size_t nDevices = GetUSBMIDIDeviceCount();
			const u8 Payload[] = {static_cast<u8>(m_USBOutMode), static_cast<u8>(nDevices)};
			SendGolemResponse(nTransaction, static_cast<u8>(TGolemResponse::Ready), nCommand, Payload, sizeof(Payload));
			return true;
		}

		case TGolemCommand::SetParameter:
		{
			// Payload: parameter, value (0x7F = report only). READY carries
			// the parameter and its value. Settings last until power-off.
			if (nSize != GolemHeaderSize + 3)
			{
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::MalformedRequest));
				return true;
			}

			const auto Parameter = static_cast<TGolemParameter>(pData[8]);
			const u8 nValue      = pData[9];
			const bool bQuery    = nValue == GolemParameterQuery;
			u8 nMax;

			switch (Parameter)
			{
				case TGolemParameter::Volume:       nMax = 100; break;
				case TGolemParameter::Effects:
				case TGolemParameter::MIDIChannels:
				case TGolemParameter::USBInThru:
				case TGolemParameter::KeysToNext:
				case TGolemParameter::Loudness:     nMax = 1;   break;
				case TGolemParameter::OPL3Bank:     nMax = static_cast<u8>(m_pOPL3Synth ? m_pOPL3Synth->GetBankCount() - 1 : 127); break;
				default:
					SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::InvalidParameter));
					return true;
			}

			if (!bQuery && nValue > nMax)
			{
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::InvalidParameter));
				return true;
			}

			if ((Parameter == TGolemParameter::Effects && !m_pSoundFontSynth) ||
			    (Parameter == TGolemParameter::MIDIChannels && !m_pMT32Synth) ||
			    (Parameter == TGolemParameter::OPL3Bank && !m_pOPL3Synth))
			{
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::Unavailable));
				return true;
			}

			SendGolemResponse(nTransaction, static_cast<u8>(TGolemResponse::Accepted), nCommand);

			u8 nCurrent = 0;
			switch (Parameter)
			{
				case TGolemParameter::Volume:
					if (!bQuery)
						SetMasterVolume(nValue);
					nCurrent = m_nMasterVolume;
					break;

				case TGolemParameter::Effects:
					if (!bQuery)
					{
						m_pSoundFontSynth->SetEffects(nValue);
						LCDLog(TLCDLogType::Notice, "Effects: %s", nValue ? "on" : "off");
					}
					nCurrent = m_pSoundFontSynth->GetEffects();
					break;

				case TGolemParameter::MIDIChannels:
					if (!bQuery)
					{
						m_pMT32Synth->SetMIDIChannels(nValue ? CMT32Synth::TMIDIChannels::Alternate : CMT32Synth::TMIDIChannels::Standard);
						LCDLog(TLCDLogType::Notice, "MT-32 ch: %s", nValue ? "1-8,10" : "2-10");
					}
					nCurrent = m_pMT32Synth->GetMIDIChannels() == CMT32Synth::TMIDIChannels::Alternate;
					break;

				case TGolemParameter::USBInThru:
					if (!bQuery)
					{
						m_pConfig->MIDIUSBInThru = nValue;
						LCDLog(TLCDLogType::Notice, "USB in thru: %s", nValue ? "on" : "off");
					}
					nCurrent = m_pConfig->MIDIUSBInThru;
					break;

				case TGolemParameter::KeysToNext:
					if (!bQuery)
					{
						m_pConfig->GolemKeysToNext = nValue;
						LCDLog(TLCDLogType::Notice, "Keys to Next: %s", nValue ? "on" : "off");
					}
					nCurrent = m_pConfig->GolemKeysToNext;
					break;

				case TGolemParameter::Loudness:
					if (!bQuery)
					{
						m_bLoudness = nValue;
						LCDLog(TLCDLogType::Notice, "Loudness: %s", nValue ? "on" : "off");
					}
					nCurrent = m_bLoudness;
					break;

				case TGolemParameter::OPL3Bank:
					if (!bQuery && m_pOPL3Synth->SetBank(nValue))
						LCDLog(TLCDLogType::Notice, "OPL3 bank %d", nValue);
					nCurrent = static_cast<u8>(m_pOPL3Synth->GetBank());
					break;
			}

			const u8 Payload[] = {static_cast<u8>(Parameter), nCurrent};
			SendGolemResponse(nTransaction, static_cast<u8>(TGolemResponse::Ready), nCommand, Payload, sizeof(Payload));
			return true;
		}

		case TGolemCommand::Save:
		{
			// Payload: 0 = write golem.cfg, 1 = delete it. READY echoes it.
			if (nSize != GolemHeaderSize + 2)
			{
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::MalformedRequest));
				return true;
			}

			const u8 nAction = pData[8];
			if (nAction != GolemSaveWrite && nAction != GolemSaveClear)
			{
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::InvalidParameter));
				return true;
			}

			SendGolemResponse(nTransaction, static_cast<u8>(TGolemResponse::Accepted), nCommand);
			const bool bResult = nAction == GolemSaveWrite ? SaveGolemSettings() : ClearGolemSettings();
			if (bResult)
				SendGolemResponse(nTransaction, static_cast<u8>(TGolemResponse::Ready), nCommand, &nAction, 1);
			else
				SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::OperationFailed));
			return true;
		}

		default:
			SendGolemError(nTransaction, nCommand, static_cast<u8>(TGolemError::UnknownCommand));
			return true;
	}
}

// golem.cfg: the settings the Golem protocol can change, in mt32-pi.cfg
// syntax. Written to a temporary file first, so a failed write never leaves a
// half-written golem.cfg behind.
bool CMT32Pi::SaveGolemSettings()
{
	static const char* const USBOutNames[] = {"off", "on", "bridge"};
	static const char* const ROMSetNames[] = {"old", "new", "cm32l"};

	char Text[768];
	int n = snprintf(Text, sizeof(Text),
	                 "# golem.cfg: written by .golem save and read after mt32-pi.cfg,\n"
	                 "# whose values it overrides. .golem save clear deletes it.\n"
	                 "\n[system]\ndefault_synth = %s\n"
	                 "\n[midi]\nusb_out = %s\nusb_in_thru = %s\n",
	                 m_pCurrentSynth == m_pSoundFontSynth ? "soundfont" :
	                 m_pCurrentSynth == m_pOPL3Synth      ? "opl3" :
	                                                        "mt32",
	                 USBOutNames[static_cast<u8>(m_USBOutMode)],
	                 m_pConfig->MIDIUSBInThru ? "on" : "off");

	if (m_pMT32Synth)
	{
		const u8 nROMSet = static_cast<u8>(m_pMT32Synth->GetROMSet());
		n += snprintf(Text + n, sizeof(Text) - n,
		              "\n[mt32emu]\n%s%s%smidi_channels = %s\n",
		              nROMSet < 3 ? "rom_set = " : "",
		              nROMSet < 3 ? ROMSetNames[nROMSet] : "",
		              nROMSet < 3 ? "\n" : "",
		              m_pMT32Synth->GetMIDIChannels() == CMT32Synth::TMIDIChannels::Alternate ? "alternate" : "standard");
	}

	if (m_pSoundFontSynth)
		n += snprintf(Text + n, sizeof(Text) - n,
		              "\n[fluidsynth]\nsoundfont = %u\n",
		              static_cast<unsigned>(m_pSoundFontSynth->GetSoundFontIndex()));

	if (m_pOPL3Synth)
		n += snprintf(Text + n, sizeof(Text) - n, "\n[opl3]\nbank = %d\n", m_pOPL3Synth->GetBank());

	n += snprintf(Text + n, sizeof(Text) - n, "\n[golem]\nvolume = %d\nkeys = %s\nloudness = %s\n", m_nMasterVolume,
	              m_pConfig->GolemKeysToNext ? "on" : "off", m_bLoudness ? "on" : "off");
	if (m_pSoundFontSynth && m_pSoundFontSynth->HasEffectsSet())
		n += snprintf(Text + n, sizeof(Text) - n, "effects = %d\n", m_pSoundFontSynth->GetEffects() ? 1 : 0);

	if (n <= 0 || n >= static_cast<int>(sizeof(Text)))
		return false;

	FIL File;
	UINT nWritten = 0;
	if (f_open(&File, GolemConfigTempPath, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
	{
		LOGERR("golem.cfg: cannot create %s", GolemConfigTempPath);
		return false;
	}
	const FRESULT WriteResult = f_write(&File, Text, n, &nWritten);
	const FRESULT CloseResult = f_close(&File);
	if (WriteResult != FR_OK || CloseResult != FR_OK || nWritten != static_cast<UINT>(n))
	{
		LOGERR("golem.cfg: write failed");
		f_unlink(GolemConfigTempPath);
		return false;
	}

	// FatFs cannot rename over an existing file
	const FRESULT UnlinkResult = f_unlink(GolemConfigPath);
	if (UnlinkResult != FR_OK && UnlinkResult != FR_NO_FILE)
		return false;
	if (f_rename(GolemConfigTempPath, GolemConfigPath) != FR_OK)
	{
		LOGERR("golem.cfg: rename failed");
		return false;
	}

	LOGNOTE("Settings saved to golem.cfg");
	LCDLog(TLCDLogType::Notice, "Settings saved");
	return true;
}

bool CMT32Pi::ClearGolemSettings()
{
	const FRESULT Result = f_unlink(GolemConfigPath);
	if (Result != FR_OK && Result != FR_NO_FILE)
	{
		LOGERR("golem.cfg: cannot delete");
		return false;
	}

	LOGNOTE("golem.cfg deleted");
	LCDLog(TLCDLogType::Notice, "Saved settings cleared");
	return true;
}

bool CMT32Pi::SendGolemResponse(u8 nTransaction, u8 nResponse, u8 nCommand, const u8* pPayload, size_t nPayloadSize)
{
	if (!m_bSerialMIDIEnabled || m_pSerial == nullptr || nPayloadSize > GolemMaxPayload)
		return false;

	u8 Message[GolemHeaderSize + GolemMaxPayload + 2];
	size_t nSize = 0;
	Message[nSize++] = 0xF0;
	Message[nSize++] = 0x7D;
	Message[nSize++] = GolemSignature[0];
	Message[nSize++] = GolemSignature[1];
	Message[nSize++] = GolemSignature[2];
	Message[nSize++] = GolemProtocolVersion;
	Message[nSize++] = nTransaction;
	Message[nSize++] = nResponse;
	Message[nSize++] = nCommand;

	for (size_t i = 0; i < nPayloadSize; ++i)
		Message[nSize++] = pPayload[i] & 0x7F;

	Message[nSize++] = 0xF7;

	const int nResult = m_pSerial->Write(Message, nSize);
	if (nResult != static_cast<int>(nSize))
	{
		LOGERR("Golem response: sent %d of %d bytes", nResult, static_cast<int>(nSize));
		return false;
	}

	return true;
}

void CMT32Pi::SendGolemError(u8 nTransaction, u8 nCommand, u8 nError)
{
	SendGolemResponse(nTransaction, static_cast<u8>(TGolemResponse::Error), nCommand, &nError, 1);
}

void CMT32Pi::SendGolemStatus(u8 nTransaction)
{
	constexpr u16 Capabilities = GolemCapabilityStatus |
	                             GolemCapabilitySetSynth |
	                             GolemCapabilitySetROMSet |
	                             GolemCapabilitySetSoundFont |
	                             GolemCapabilitySetUSBOut |
	                             GolemCapabilitySetParameter |
	                             GolemCapabilitySave;

	u8 nAvailability = 0;
	if (m_pMT32Synth)
		nAvailability |= 1 << 0;
	if (m_pSoundFontSynth)
		nAvailability |= 1 << 1;
	if (m_pOPL3Synth)
		nAvailability |= 1 << 2;

	u8 nSynth = 0x7F;
	if (m_pCurrentSynth == m_pMT32Synth && m_pMT32Synth)
		nSynth = static_cast<u8>(TSynth::MT32);
	else if (m_pCurrentSynth == m_pSoundFontSynth && m_pSoundFontSynth)
		nSynth = static_cast<u8>(TSynth::SoundFont);
	else if (m_pCurrentSynth == m_pOPL3Synth && m_pOPL3Synth)
		nSynth = static_cast<u8>(TSynth::OPL3);

	u8 nROMSet = 0x7F;
	if (m_pMT32Synth)
		nROMSet = static_cast<u8>(m_pMT32Synth->GetROMSet());

	size_t nSoundFont = 0x3FFF;
	if (m_pSoundFontSynth)
		nSoundFont = m_pSoundFontSynth->GetSoundFontIndex();

	const u8 Payload[] = {
		static_cast<u8>(Capabilities & 0x7F),
		static_cast<u8>((Capabilities >> 7) & 0x7F),
		nAvailability,
		nSynth,
		nROMSet,
		static_cast<u8>(nSoundFont & 0x7F),
		static_cast<u8>((nSoundFont >> 7) & 0x7F),
	};

	SendGolemResponse(nTransaction,
	                  static_cast<u8>(TGolemResponse::Status),
	                  static_cast<u8>(TGolemCommand::GetStatus),
	                  Payload,
	                  sizeof(Payload));
}

void CMT32Pi::UpdateUSB(bool bStartup)
{
	if (!m_bUSBAvailable || !m_pUSBHCI->UpdatePlugAndPlay())
		return;

	Awaken();

	CUSBBulkOnlyMassStorageDevice* pUSBMassStorageDevice = static_cast<CUSBBulkOnlyMassStorageDevice*>(CDeviceNameService::Get()->GetDevice("umsd1", TRUE));

	if (!m_pUSBMassStorageDevice && pUSBMassStorageDevice)
	{
		// USB disk was attached
		LOGNOTE("USB mass storage device attached");

		if (f_mount(&m_USBFileSystem, "USB:", 1) != FR_OK)
			LOGERR("Failed to mount USB mass storage device");
		else
		{
			if (!bStartup)
			{
				LCDLog(TLCDLogType::Spinner, "MT-32 ROM rescan");
				if (m_pMT32Synth)
					m_pMT32Synth->GetROMManager().ScanROMs();
				else
					InitMT32Synth();

				LCDLog(TLCDLogType::Spinner, "SoundFont rescan");
				if (m_pSoundFontSynth)
					m_pSoundFontSynth->GetSoundFontManager().ScanSoundFonts();
				else
					InitSoundFontSynth();

				if (m_pSoundFontSynth)
					LCDLog(TLCDLogType::Notice, "%d SoundFonts avail", m_pSoundFontSynth->GetSoundFontManager().GetSoundFontCount());
			}
		}
	}
	else if (m_pUSBMassStorageDevice && !pUSBMassStorageDevice)
	{
		// USB disk was removed
		LOGNOTE("USB mass storage device removed");

		f_unmount("USB:");

		// Only need to rescan SoundFonts on storage removal; MT-32 ROMs are kept in memory
		if (m_pSoundFontSynth)
		{
			LCDLog(TLCDLogType::Spinner, "SoundFont rescan");
			m_pSoundFontSynth->GetSoundFontManager().ScanSoundFonts();
			LCDLog(TLCDLogType::Notice, "%d SoundFonts avail", m_pSoundFontSynth->GetSoundFontManager().GetSoundFontCount());
		}
	}
	m_pUSBMassStorageDevice = pUSBMassStorageDevice;

	// USB MIDI devices are parsed separately from GPIO serial MIDI, so the
	// Next keeps working (and receiving Golem replies) while a USB interface
	// or keyboard is attached.
	for (size_t i = 0; i < MaxUSBMIDIDevices; ++i)
	{
		if (m_pUSBMIDIDevices[i])
			continue;

		CString DeviceName;
		DeviceName.Format("umidi%u", static_cast<unsigned>(i + 1));
		CUSBMIDIDevice* pDevice = static_cast<CUSBMIDIDevice*>(CDeviceNameService::Get()->GetDevice(DeviceName, FALSE));
		if (!pDevice)
			continue;

		m_pUSBMIDIDevices[i] = pDevice;
		pDevice->RegisterRemovedHandler(USBMIDIDeviceRemovedHandler, &m_pUSBMIDIDevices[i]);
		pDevice->RegisterPacketHandler(USBMIDIPacketHandler);
		LOGNOTE("Using USB MIDI interface %s", static_cast<const char*>(DeviceName));
		LCDLog(TLCDLogType::Notice, "USB MIDI %u connected", static_cast<unsigned>(i + 1));
	}

	if (!m_pUSBSerialDevice && (m_pUSBSerialDevice = static_cast<CUSBSerialDevice*>(CDeviceNameService::Get()->GetDevice("utty1", FALSE))))
	{
		m_pUSBSerialDevice->SetBaudRate(m_pConfig->MIDIUSBSerialBaudRate);
		m_pUSBSerialDevice->RegisterRemovedHandler(USBMIDIDeviceRemovedHandler, &m_pUSBSerialDevice);
		LOGNOTE("Using USB serial interface");
	}
}

void CMT32Pi::UpdateNetwork()
{
	if (!m_pNet)
		return;

	bool bNetIsRunning = m_pNet->IsRunning();
	if (m_pConfig->NetworkMode == CConfig::TNetworkMode::Ethernet)
		bNetIsRunning &= m_pNetDevice->IsLinkUp();
	else if (m_pConfig->NetworkMode == CConfig::TNetworkMode::WiFi)
		bNetIsRunning &= m_WPASupplicant.IsConnected();

	if (!m_bNetworkReady && bNetIsRunning)
	{
		m_bNetworkReady = true;

		CString IPString;
		m_pNet->GetConfig()->GetIPAddress()->Format(&IPString);

		LOGNOTE("Network up and running at: %s", static_cast<const char *>(IPString));
		LCDLog(TLCDLogType::Notice, "%s: %s", GetNetworkDeviceShortName(), static_cast<const char*>(IPString));

		if (m_pConfig->NetworkRTPMIDI && !m_pAppleMIDIParticipant)
		{
			m_pAppleMIDIParticipant = new CAppleMIDIParticipant(&m_Random, this);
			if (!m_pAppleMIDIParticipant->Initialize())
			{
				LOGERR("Failed to init AppleMIDI receiver");
				delete m_pAppleMIDIParticipant;
				m_pAppleMIDIParticipant = nullptr;
			}
			else
				LOGNOTE("AppleMIDI receiver initialized");
		}

		if (m_pConfig->NetworkUDPMIDI && !m_pUDPMIDIReceiver)
		{
			m_pUDPMIDIReceiver = new CUDPMIDIReceiver(this);
			if (!m_pUDPMIDIReceiver->Initialize())
			{
				LOGERR("Failed to init UDP MIDI receiver");
				delete m_pUDPMIDIReceiver;
				m_pUDPMIDIReceiver = nullptr;
			}
			else
				LOGNOTE("UDP MIDI receiver initialized");
		}

		if (m_pConfig->NetworkFTPServer && !m_pFTPDaemon)
		{
			m_pFTPDaemon = new CFTPDaemon(m_pConfig->NetworkFTPUsername, m_pConfig->NetworkFTPPassword);
			if (!m_pFTPDaemon->Initialize())
			{
				LOGERR("Failed to init FTP daemon");
				delete m_pFTPDaemon;
				m_pFTPDaemon = nullptr;
			}
			else
				LOGNOTE("FTP daemon initialized");
		}
	}
	else if (m_bNetworkReady && !bNetIsRunning)
	{
		m_bNetworkReady = false;
		LOGNOTE("Network disconnected.");
		LCDLog(TLCDLogType::Notice, "%s disconnected!", GetNetworkDeviceShortName());

	}
}

void CMT32Pi::UpdateMIDI()
{
	size_t nBytes;
	bool bReceived = false;
	u8 Buffer[MIDIRxBufferSize];

	// Host MIDI: GPIO serial (the Next)
	if (m_bSerialMIDIEnabled && (nBytes = ReceiveSerialMIDI(Buffer, sizeof(Buffer))) > 0)
	{
		ParseMIDIBytes(Buffer, nBytes);
		bReceived = true;
	}

	// External MIDI: USB serial, then USB MIDI/Pisound ring buffer, each
	// with its own parser so a message split across reads is never mixed
	// with bytes from the other source
	if (m_pUSBSerialDevice)
	{
		const int nResult = m_pUSBSerialDevice->Read(Buffer, sizeof(Buffer));
		if (nResult > 0)
		{
			m_USBSerialMIDIParser.ParseMIDIBytes(Buffer, static_cast<size_t>(nResult));
			bReceived = true;
		}
	}

	if ((nBytes = m_MIDIRxBuffer.Dequeue(Buffer, sizeof(Buffer))) > 0)
	{
		m_ExternalMIDIParser.ParseMIDIBytes(Buffer, nBytes);
		bReceived = true;
	}

	// Reset the Active Sense timer
	if (bReceived)
		m_nActiveSenseTime = m_pTimer->GetTicks();
}

void CMT32Pi::PurgeMIDIBuffers()
{
	size_t nBytes;
	u8 Buffer[MIDIRxBufferSize];

	// Process MIDI messages from all devices/ring buffers, but ignore note-ons
	while (m_bSerialMIDIEnabled && (nBytes = ReceiveSerialMIDI(Buffer, sizeof(Buffer))) > 0)
		ParseMIDIBytes(Buffer, nBytes, true);

	int nResult;
	while (m_pUSBSerialDevice && (nResult = m_pUSBSerialDevice->Read(Buffer, sizeof(Buffer))) > 0)
		m_USBSerialMIDIParser.ParseMIDIBytes(Buffer, static_cast<size_t>(nResult), true);

	while ((nBytes = m_MIDIRxBuffer.Dequeue(Buffer, sizeof(Buffer))) > 0)
		m_ExternalMIDIParser.ParseMIDIBytes(Buffer, nBytes, true);
}

size_t CMT32Pi::ReceiveSerialMIDI(u8* pOutData, size_t nSize)
{
	// Read serial MIDI data
	int nResult = m_pSerial->Read(pOutData, nSize);

	// No data
	if (nResult == 0)
		return 0;

	// Error
	if (nResult < 0)
	{
		if (m_pConfig->SystemVerbose)
		{
			const char* pErrorString;
			switch (nResult)
			{
				case -SERIAL_ERROR_BREAK:
					pErrorString = "UART break error!";
					break;

				case -SERIAL_ERROR_OVERRUN:
					pErrorString = "UART overrun error!";
					break;

				case -SERIAL_ERROR_FRAMING:
					pErrorString = "UART framing error!";
					break;

				default:
					pErrorString = "Unknown UART error!";
					break;
			}

			LOGWARN(pErrorString);
			LCDLog(TLCDLogType::Warning, pErrorString);
		}

		return 0;
	}

	// Replay received MIDI data out via the serial port ('software thru')
	if (m_pConfig->MIDIGPIOThru)
	{
		int nSendResult = m_pSerial->Write(pOutData, nResult);
		if (nSendResult != nResult)
		{
			LOGERR("received %d bytes, but only sent %d bytes", nResult, nSendResult);
			LCDLog(TLCDLogType::Error, "UART TX error!");
		}
	}

	return static_cast<size_t>(nResult);
}

void CMT32Pi::ProcessEventQueue()
{
	TEvent Buffer[EventQueueSize];
	const size_t nEvents = m_EventQueue.Dequeue(Buffer, sizeof(Buffer));

	// We got some events, wake up
	if (nEvents > 0)
		Awaken();

	for (size_t i = 0; i < nEvents; ++i)
	{
		const TEvent& Event = Buffer[i];

		switch (Event.Type)
		{
			case TEventType::Button:
				ProcessButtonEvent(Event.Button);
				break;

			case TEventType::SwitchSynth:
				SwitchSynth(Event.SwitchSynth.Synth);
				break;

			case TEventType::SwitchMT32ROMSet:
				SwitchMT32ROMSet(Event.SwitchMT32ROMSet.ROMSet);
				break;

			case TEventType::SwitchSoundFont:
				DeferSwitchSoundFont(Event.SwitchSoundFont.Index);
				break;

			case TEventType::AllSoundOff:
				if (m_pMT32Synth)
					m_pMT32Synth->AllSoundOff();
				if (m_pSoundFontSynth)
					m_pSoundFontSynth->AllSoundOff();
				if (m_pOPL3Synth)
					m_pOPL3Synth->AllSoundOff();
				break;

			case TEventType::DisplayImage:
				m_UserInterface.DisplayImage(Event.DisplayImage.Image);
				break;

			case TEventType::Encoder:
				SetMasterVolume(m_nMasterVolume + Event.Encoder.nDelta);
				break;
		}
	}
}

void CMT32Pi::ProcessButtonEvent(const TButtonEvent& Event)
{
	if (Event.Button == TButton::EncoderButton)
	{
		LCDLog(TLCDLogType::Notice, "Enc. button %s", Event.bPressed ? "PRESSED" : "RELEASED");
		return;
	}

	if (!Event.bPressed)
		return;

	if (Event.Button == TButton::Button1 && !Event.bRepeat)
	{
		// Next available synth: MT-32, SoundFont, OPL3
		CSynthBase* const Synths[] = {m_pMT32Synth, m_pSoundFontSynth, m_pOPL3Synth};
		size_t nCurrent = 0;
		while (nCurrent < 3 && Synths[nCurrent] != m_pCurrentSynth)
			++nCurrent;
		for (size_t i = 1; i < 3; ++i)
		{
			const size_t nNext = (nCurrent + i) % 3;
			if (Synths[nNext])
			{
				SwitchSynth(static_cast<TSynth>(nNext));
				break;
			}
		}
	}
	else if (Event.Button == TButton::Button2 && !Event.bRepeat)
	{
		if (m_pCurrentSynth == m_pMT32Synth)
			NextMT32ROMSet();
		else if (m_pCurrentSynth == m_pOPL3Synth)
		{
			// Next OPL3 bank
			const int nBank = (m_pOPL3Synth->GetBank() + 1) % m_pOPL3Synth->GetBankCount();
			if (m_pOPL3Synth->SetBank(nBank))
				m_pOPL3Synth->ReportStatus();
		}
		else
		{
			// Next SoundFont
			const size_t nSoundFonts = m_pSoundFontSynth->GetSoundFontManager().GetSoundFontCount();

			if (!nSoundFonts)
				LCDLog(TLCDLogType::Error, "No SoundFonts!");
			else
			{
				size_t nNextSoundFont;
				if (m_bDeferredSoundFontSwitchFlag)
					nNextSoundFont = (m_nDeferredSoundFontSwitchIndex + 1) % nSoundFonts;
				else
				{
					// Current SoundFont was probably on a USB stick that has since been removed
					const size_t nCurrentSoundFont = m_pSoundFontSynth->GetSoundFontIndex();
					if (nCurrentSoundFont > nSoundFonts)
						nNextSoundFont = 0;
					else
						nNextSoundFont = (nCurrentSoundFont + 1) % nSoundFonts;
				}

				DeferSwitchSoundFont(nNextSoundFont);
			}
		}
	}
	else if (Event.Button == TButton::Button3)
	{
		SetMasterVolume(m_nMasterVolume - 1);
	}
	else if (Event.Button == TButton::Button4)
	{
		SetMasterVolume(m_nMasterVolume + 1);
	}
}

void CMT32Pi::SwitchSynth(TSynth NewSynth)
{
	CSynthBase* pNewSynth = nullptr;

	if (NewSynth == TSynth::MT32)
		pNewSynth = m_pMT32Synth;
	else if (NewSynth == TSynth::SoundFont)
		pNewSynth = m_pSoundFontSynth;
	else if (NewSynth == TSynth::OPL3)
		pNewSynth = m_pOPL3Synth;

	if (pNewSynth == nullptr)
	{
		LCDLog(TLCDLogType::Warning, "Synth unavailable!");
		return;
	}

	if (pNewSynth == m_pCurrentSynth)
	{
		LCDLog(TLCDLogType::Warning, "Already active!");
		return;
	}

	m_pCurrentSynth->AllSoundOff();
	m_pCurrentSynth = pNewSynth;
	const char* pMode = NewSynth == TSynth::MT32      ? "MT-32 mode" :
	                    NewSynth == TSynth::SoundFont ? "SoundFont mode" :
	                                                    "OPL3 mode";
	LOGNOTE("Switching to %s", pMode);
	LCDLog(TLCDLogType::Notice, pMode);
}

void CMT32Pi::SwitchMT32ROMSet(TMT32ROMSet ROMSet)
{
	if (m_pMT32Synth == nullptr)
		return;

	LOGNOTE("Switching to ROM set %d", static_cast<u8>(ROMSet));
	if (m_pMT32Synth->SwitchROMSet(ROMSet) && m_pCurrentSynth == m_pMT32Synth)
		m_pMT32Synth->ReportStatus();
}

void CMT32Pi::NextMT32ROMSet()
{
	if (m_pMT32Synth == nullptr)
		return;

	LOGNOTE("Switching to next ROM set");

	if (m_pMT32Synth->NextROMSet() && m_pCurrentSynth == m_pMT32Synth)
		m_pMT32Synth->ReportStatus();
}

void CMT32Pi::SwitchSoundFont(size_t nIndex)
{
	if (m_pSoundFontSynth == nullptr)
		return;

	LOGNOTE("Switching to SoundFont %d", nIndex);
	if (m_pSoundFontSynth->SwitchSoundFont(nIndex))
	{
		// Handle any MIDI data that has been queued up while busy
		PurgeMIDIBuffers();

		if (m_pCurrentSynth == m_pSoundFontSynth)
			m_pSoundFontSynth->ReportStatus();
	}
}

void CMT32Pi::DeferSwitchSoundFont(size_t nIndex)
{
	if (m_pSoundFontSynth == nullptr)
		return;

	const char* pName = m_pSoundFontSynth->GetSoundFontManager().GetSoundFontName(nIndex);
	LCDLog(TLCDLogType::Notice, "SF %ld: %s", nIndex, pName ? pName : "- N/A -");
	m_nDeferredSoundFontSwitchIndex = nIndex;
	m_nDeferredSoundFontSwitchTime  = CTimer::Get()->GetTicks();
	m_bDeferredSoundFontSwitchFlag  = true;
}

void CMT32Pi::SetMasterVolume(s32 nVolume)
{
	m_nMasterVolume = Utility::Clamp(nVolume, 0, 100);

	if (m_pMT32Synth)
		m_pMT32Synth->SetMasterVolume(m_nMasterVolume);
	if (m_pSoundFontSynth)
		m_pSoundFontSynth->SetMasterVolume(m_nMasterVolume);
	if (m_pOPL3Synth)
		m_pOPL3Synth->SetMasterVolume(m_nMasterVolume);

	if (m_pCurrentSynth == m_pSoundFontSynth || m_pCurrentSynth == m_pOPL3Synth)
		LCDLog(TLCDLogType::Notice, "Volume: %d", m_nMasterVolume);
}

void CMT32Pi::LEDOn()
{
	m_pActLED->On();
	m_nLEDOnTime = m_pTimer->GetTicks();
	m_bLEDOn = true;
}

void CMT32Pi::LCDLog(TLCDLogType Type, const char* pFormat...)
{
	if (!m_pLCD)
		return;

	char Buffer[256];

	va_list Args;
	va_start(Args, pFormat);
	vsnprintf(Buffer, sizeof(Buffer), pFormat, Args);
	va_end(Args);

	// LCD task hasn't started yet; print directly
	if (Type == TLCDLogType::Startup)
	{
		if (m_pLCD->GetType() == CLCD::TType::Graphical && !m_pConfig->SystemVerbose)
			return;

		u8 nOffsetX = CUserInterface::CenterMessageOffset(*m_pLCD, Buffer);
		m_pLCD->Print(Buffer, nOffsetX, 1, true, true);
	}

	// Let LCD task pick up the message in its next update
	else
		m_UserInterface.ShowSystemMessage(Buffer, Type == TLCDLogType::Spinner);
}

const char* CMT32Pi::GetNetworkDeviceShortName() const
{
	switch (m_pConfig->NetworkMode)
	{
		case CConfig::TNetworkMode::Ethernet:	return "Ether";
		case CConfig::TNetworkMode::WiFi:	return "WiFi";
		default:				return "None";
	}
}

void CMT32Pi::EventHandler(const TEvent& Event)
{
	assert(s_pThis != nullptr);

	// Enqueue event
	s_pThis->m_EventQueue.Enqueue(Event);
}

void CMT32Pi::USBMIDIDeviceRemovedHandler(CDevice* pDevice, void* pContext)
{
	assert(s_pThis != nullptr);

	void** pDevicePointer = reinterpret_cast<void**>(pContext);
	*pDevicePointer = nullptr;

	LOGNOTE("USB MIDI device removed");
}

// The following handlers are called from interrupt context, enqueue into ring buffer for main thread
void CMT32Pi::USBMIDIPacketHandler(unsigned nCable, u8* pPacket, unsigned nLength)
{
	IRQMIDIReceiveHandler(pPacket, nLength);
}

void CMT32Pi::IRQMIDIReceiveHandler(const u8* pData, size_t nSize)
{
	assert(s_pThis != nullptr);

	// Enqueue data into ring buffer
	if (s_pThis->m_MIDIRxBuffer.Enqueue(pData, nSize) != nSize)
	{
		static const char* pErrorString = "MIDI overrun error!";
		LOGWARN(pErrorString);
		s_pThis->LCDLog(TLCDLogType::Error, pErrorString);
	}
}

void CMT32Pi::PanicHandler()
{
	if (!s_pThis || !s_pThis->m_pLCD)
		return;

	// Kill UI task
	s_pThis->m_bRunning = false;
	while (!s_pThis->m_bUITaskDone)
		;

	const char* pGuru = "Guru Meditation:";
	u8 nOffsetX = CUserInterface::CenterMessageOffset(*s_pThis->m_pLCD, pGuru);
	s_pThis->m_pLCD->Clear(true);
	s_pThis->m_pLCD->Print(pGuru, nOffsetX, 0, true, true);

	char Buffer[LOGGER_BUFSIZE];
	CLogger::Get()->Read(Buffer, sizeof(Buffer), false);

	// Find last newline
	char* pMessageStart = strrchr(Buffer, '\n');
	if (!pMessageStart)
		return;
	*pMessageStart = '\0';

	// Find second-last newline
	pMessageStart = strrchr(Buffer, '\n');
	if (!pMessageStart)
		return;

	// Skip past timestamp and log source, kill color control characters
	pMessageStart = strstr(pMessageStart, ": ") + 2;
	char* pMessageEnd = strstr(pMessageStart, "\x1B[0m");
	*pMessageEnd = '\0';

	const size_t nMessageLength = strlen(pMessageStart);
	size_t nCurrentScrollOffset = 0;
	const char* pGuruFlash = pGuru;
	bool bFlash = false;

	unsigned int nTicks = CTimer::GetClockTicks();
	unsigned int nPanicStart = nTicks;
	unsigned int nFlashTime = nTicks;
	unsigned int nScrollTime = nTicks;

	const u8 nWidth = s_pThis->m_pLCD->Width();
	const u8 nHeight = s_pThis->m_pLCD->Height();

	// TODO: API for getting width in pixels/characters for a string
	const bool bGraphical = s_pThis->m_pLCD->GetType() == CLCD::TType::Graphical;
	const size_t nCharWidth = bGraphical ? 20 : nWidth;

	while (true)
	{
		s_pThis->m_pLCD->Clear(false);
		nTicks = CTimer::GetClockTicks();

		if (Utility::TicksToMillis(nTicks - nFlashTime) > 1000)
		{
			bFlash = !bFlash;
			nFlashTime = nTicks;
		}

		if (nMessageLength > nCharWidth)
		{
			if (nMessageLength - nCurrentScrollOffset > nCharWidth)
			{
				const unsigned int nTimeout = nCurrentScrollOffset == 0 ? 1500 : 175;
				if (Utility::TicksToMillis(nTicks - nScrollTime) >= nTimeout)
				{
					++nCurrentScrollOffset;
					nScrollTime = nTicks;
				}
			}
			else if (Utility::TicksToMillis(nTicks - nScrollTime) >= 3000)
			{
				nCurrentScrollOffset = 0;
				nScrollTime = nTicks;
			}
		}

		if (Utility::TicksToMillis(nTicks - nPanicStart) > 2 * 60 * 1000)
			break;

		if (!bGraphical)
			pGuruFlash = bFlash ? "" : pGuru;

		u8 nOffsetX = CUserInterface::CenterMessageOffset(*s_pThis->m_pLCD, pGuruFlash);
		s_pThis->m_pLCD->Print(pGuruFlash, nOffsetX, 0, true, false);
		s_pThis->m_pLCD->Print(pMessageStart + nCurrentScrollOffset, 0, 1, true, false);

		if (bGraphical && bFlash)
		{
			s_pThis->m_pLCD->DrawFilledRect(0, 0, nWidth - 1, 1);
			s_pThis->m_pLCD->DrawFilledRect(0, nHeight - 1, nWidth - 1, nHeight - 2);
			s_pThis->m_pLCD->DrawFilledRect(0, 0, 1, nHeight - 1);
			s_pThis->m_pLCD->DrawFilledRect(nWidth - 1, 0, nWidth - 2, nHeight - 1);
		}

		s_pThis->m_pLCD->Flip();
	}

	s_pThis->m_pLCD->Clear(true);
	const char* pMessage = "System halted";
	nOffsetX = CUserInterface::CenterMessageOffset(*s_pThis->m_pLCD, pMessage);
	s_pThis->m_pLCD->Print(pMessage, nOffsetX, 0, true, true);
	pMessage = "Please reboot";
	nOffsetX = CUserInterface::CenterMessageOffset(*s_pThis->m_pLCD, pMessage);
	s_pThis->m_pLCD->Print(pMessage, nOffsetX, 1, true, true);
}
