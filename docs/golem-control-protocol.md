# Golem control protocol v1

This fork extends mt32-pi with a small bidirectional control protocol for the
MT32-Next/Golem client. Normal MIDI traffic and the original mt32-pi custom
SysEx commands remain unchanged.

The protocol is experimental. It currently uses the MIDI educational/development
manufacturer ID `0x7D`; that ID must be reviewed before a commercial wire
protocol is declared stable.

## Transport

Requests travel from the ZX Spectrum Next to the Raspberry Pi over the existing
GPIO serial MIDI connection. Replies travel over the reverse UART connection,
so Pi TX to Next RX and a common ground are required.

Replies are emitted only when the serial MIDI interface is active. Applications
must use bounded timeouts and must never wait forever for a response.

## Envelope

All values inside a SysEx message are 7-bit bytes.

Request:

```text
F0 7D 47 4C 4D vv tt cc [payload...] F7
```

Response:

```text
F0 7D 47 4C 4D vv tt rr cc [payload...] F7
```

| Field | Meaning |
| --- | --- |
| `47 4C 4D` | ASCII signature `GLM` |
| `vv` | Protocol version; currently `01` |
| `tt` | Transaction ID, `00`–`7F` |
| `cc` | Command |
| `rr` | Response type |

The client chooses the transaction ID and ignores replies belonging to older
transactions. Set operations are idempotent: requesting the already-active
value completes successfully.

## Commands

| Value | Command | Payload |
| --- | --- | --- |
| `00` | `GET_STATUS` | none |
| `01` | `SET_SYNTH` | `00` MT-32/Munt, `01` SoundFont/FluidSynth, `02` OPL3/libADLMIDI |
| `02` | `SET_ROM_SET` | `00` MT-32 old, `01` MT-32 new, `02` CM-32L |
| `03` | `SET_SOUNDFONT` | 14-bit index as `low7 high7`; currently 0–511 |
| `04` | `SET_USB_OUT` | `00` off, `01` on, `02` bridge, `7F` report only |

Example: select SoundFont 511 using transaction 18 (`0x12`):

```text
F0 7D 47 4C 4D 01 12 03 7F 03 F7
```

## Responses

| Value | Response | Meaning |
| --- | --- | --- |
| `40` | `ACCEPTED` | Valid request accepted; work may still be in progress |
| `41` | `READY` | Requested state is active and usable |
| `42` | `ERROR` | Request failed; first payload byte is an error code |
| `43` | `STATUS` | Current capabilities and state |

Set commands first return `ACCEPTED`. They return `READY` only after the synth,
ROM set, or SoundFont switch has completed successfully. A failed operation
returns `ERROR` instead. `GET_STATUS` returns `ACCEPTED` followed by `STATUS`.

`READY` echoes the active value using the same payload representation as the
request.

## USB MIDI output

`SET_USB_OUT` controls whether MIDI from the Next (and network MIDI) is also
sent to every USB MIDI device attached to the Pi:

| Mode | Built-in synth | USB MIDI devices |
| --- | --- | --- |
| `00` off | plays | receive nothing |
| `01` on | plays | receive the same MIDI |
| `02` bridge | silent | receive the MIDI |

Forwarding includes channel messages, system common and real-time messages, and
every SysEx except the mt32-pi and Golem control messages, which the Pi always
consumes. Switching to off sends All Sound Off and All Notes Off on all 16
channels to the USB devices; entering or leaving bridge silences the built-in
synth.

`READY` carries two bytes: the active mode and the number of attached USB MIDI
devices (`00` if none). `7F` changes nothing and only reports, so a client can
read the mode without `STATUS` growing. The boot-time mode comes from `usb_out`
in `mt32-pi.cfg`.

MIDI from USB devices (a keyboard, a USB-to-DIN interface's input) is parsed
separately from the GPIO stream, always reaches the built-in synth, and is
forwarded to USB devices only when `usb_in_thru = on`. Golem requests arriving
over USB are ignored, because replies always go to the GPIO serial port.

Each byte stream has its own MIDI parser, so a message split across reads is
never mixed with bytes from another source: GPIO serial, USB serial, and USB
MIDI/Pisound. Known limits:

- All USB MIDI devices share one parser, because Circle does not report which
  device a packet came from. Short messages are safe (each USB packet carries
  a whole message), but a SysEx from one device can be broken by another
  device sending at the same moment.
- Network MIDI (RTP-MIDI, UDP) shares the GPIO parser, as in upstream
  mt32-pi. It only matters when the network is enabled in `mt32-pi.cfg`.
- All sources play the same 16 channels: a keyboard on a channel the song
  uses mixes with it, and the song's cleanup (All Notes Off) also stops the
  keyboard's notes.

Example: switch to bridge using transaction 18 (`0x12`), and the replies with
one device attached:

```text
F0 7D 47 4C 4D 01 12 04 02 F7
F0 7D 47 4C 4D 01 12 40 04 F7
F0 7D 47 4C 4D 01 12 41 04 02 01 F7
```

## Error codes

| Value | Error |
| --- | --- |
| `01` | Malformed request |
| `02` | Unsupported protocol version |
| `03` | Unknown command |
| `04` | Invalid parameter |
| `05` | Requested synth, ROM, or SoundFont unavailable |
| `06` | Operation failed after being accepted |

## Status payload

`STATUS` uses command `00` and contains seven bytes:

```text
cap-low cap-high availability synth rom sf-low sf-high
```

Capabilities form a 14-bit bitmap:

| Bit | Capability |
| --- | --- |
| 0 | Status query |
| 1 | Select synthesizer |
| 2 | Select MT-32 ROM set |
| 3 | Select SoundFont |
| 4 | USB MIDI output (`SET_USB_OUT`) |
| 5 | Settings (`SET_PARAMETER`) |
| 6 | Save settings (`SAVE`) |
| 7 | Sound-parameter protocol (`GET_INFO`, commands `07` onwards) |

Availability bit 0 means MT-32/Munt is available; bit 1 means
SoundFont/FluidSynth is available; bit 2 means OPL3/libADLMIDI is available. Synth and ROM use the values listed above.
The SoundFont index is a 14-bit `low7 high7` value. Unavailable or unknown state
is represented by `7F` for synth/ROM and `7F 7F` for SoundFont.

## Reliability contract

- A valid but slow operation produces `ACCEPTED` before loading begins.
- `READY` confirms software state, not the analogue audio path or speakers.
- Clients use a short timeout for `ACCEPTED` and a longer operation-specific
  timeout for `READY`.
- A retry reuses the same transaction ID and desired value. Because commands
  set absolute state rather than toggling it, retrying is safe.
- After reconnecting or detecting an unknown state, clients issue `GET_STATUS`.
- The resident Next driver transports bytes without blocking; timeout and retry
  policy belongs in the Golem client library.

## Sound-parameter commands

Commands `07` onwards (capability bit 7) are the sound-parameter protocol,
specified in golem-next `docs/sound-params.md`. They use the same frame and
version byte; each starts with a flags byte and ends with a Roland checksum,
and they have their own replies (`44 INFO`, `45 DATA`) and error codes
(`07`-`0B`). The firmware implements `GET_INFO` (`07`), `PARAM_WRITE` (`08`),
`PARAM_READ` (`09`) and `BLOB` (`0C`) for:

- area `00`: the MT-32's memory with Roland's addresses (patch and rhythm
  temporary, timbre temporary, patch memory, timbre memory, system; display
  and reset can only be written). A write is a DT1 in order with the music; a
  read sees every write before it. Works with another engine active, to
  prepare sounds; without MT-32 ROMs, "unavailable" (`05`);
- area `01`: OPL3 instruments, 128 bytes each, of the working bank;
- area `02`: the OPL3 bank directory and header;
- area `04`: the MT-32's ROM timbres, read-only: timbre *n* (0-191, A1-B64
  then the rhythm timbres) at *n* x `02 00`, 256 bytes each;
- area `10`: settings, section `00` (the `SET_PARAMETER` values at `00`-`06`,
  read-only state at `20`-`26`), section `01` (MT-32 gain and reverb gain
  x100, resampler quality 0-4, reversed stereo, reverb 0 as the music says /
  1 forced off / 2 forced on), section `02` (FluidSynth: reverb on/off, room
  size, damping, width, level; chorus on/off, voices, level, speed, depth,
  type; polyphony; gain) and section `03` (OPL3 emulator, chips, gain);
- `BLOB` type `01`: the working bank as a WOPL file (WOPLX when it carries the
  Golem extras), read out or uploaded;
- `BLOB` type `03`: FluidSynth's effects as a profile (`.cfg`, the format of
  the SoundFont profiles plus `chorus_type` and `polyphony`): read out, what
  plays; uploaded, what plays from then on, until another SoundFont is chosen.
  `fx` (`SET_PARAMETER` 1) stays the general switch: off, no reverb and no
  chorus whatever the effects say;
- `BLOB` type `04`: an MT-32 dump, DT1 messages one after another (`.SYX`):
  read out, it holds the timbre memory, patch memory, rhythm setup and
  system; uploaded, it is checked whole and then played to the MT-32.

Native Roland RQ1 (`F0 41 10 16 11 ...`) from the GPIO serial port is
answered while the MT-32 is the active engine, as an MT-32 does: DT1 messages
of up to 256 bytes, at least 20 ms apart, with only the valid part of the
request, cut at the end of its zone. Requests to another unit, relative to a
channel, for the display or reset, or with `usb_out = bridge` get no answer.
RQ1 from USB or the network is not answered yet.

The OPL3 working bank is what the engine plays. Edits are heard at once,
survive GM System On and the stop sweep, and last until another bank is
chosen (`SET_PARAMETER` 5, `.golem bank`). The other areas and commands
answer "address" (`08`) or "unknown command" (`03`) until they exist.
