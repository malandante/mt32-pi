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
| `01` | `SET_SYNTH` | `00` MT-32/Munt, `01` SoundFont/FluidSynth |
| `02` | `SET_ROM_SET` | `00` MT-32 old, `01` MT-32 new, `02` CM-32L |
| `03` | `SET_SOUNDFONT` | 14-bit index as `low7 high7`; currently 0–511 |

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

Availability bit 0 means MT-32/Munt is available; bit 1 means
SoundFont/FluidSynth is available. Synth and ROM use the values listed above.
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
