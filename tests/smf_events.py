#!/usr/bin/env python3
"""Standard MIDI file to "ms b1 b2 b3" lines (channel messages only, all tracks
merged, tempo changes applied) for test_opl3midi's renderer.

    python3 tests/smf_events.py song.mid > song.txt
"""
import struct
import sys


def vlq(data, i):
    value = 0
    while True:
        byte = data[i]
        i += 1
        value = value << 7 | byte & 0x7F
        if byte < 0x80:
            return value, i


def events(data):
    fmt, ntracks, division = struct.unpack(">HHH", data[8:14])
    if division & 0x8000:
        raise SystemExit("SMPTE time division is not supported")
    pos = 8 + struct.unpack(">I", data[4:8])[0]
    found, tempos = [], [(0, 500000)]
    for track in range(ntracks):
        size = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        i, end, tick, status = pos + 8, pos + 8 + size, 0, 0
        pos = end
        while i < end:
            delta, i = vlq(data, i)
            tick += delta
            if data[i] & 0x80:
                status = data[i]
                i += 1
            if status == 0xFF:
                kind = data[i]
                length, i = vlq(data, i + 1)
                if kind == 0x51:
                    tempos.append((tick, int.from_bytes(data[i:i + 3], "big")))
                i += length
            elif status in (0xF0, 0xF7):
                length, i = vlq(data, i)
                i += length
            else:
                size = 1 if status & 0xF0 in (0xC0, 0xD0) else 2
                body = list(data[i:i + size])
                i += size
                found.append((tick, track, len(found), [status] + body + [0] * (2 - size)))
    tempos.sort()

    def ms(tick):
        total, last_tick, tempo = 0.0, 0, 500000
        for at, value in tempos:
            if at > tick:
                break
            total += (at - last_tick) * tempo / division / 1000
            last_tick, tempo = at, value
        return total + (tick - last_tick) * tempo / division / 1000

    found.sort(key=lambda e: (e[0], (e[3][0] & 0xF0) != 0x80 and not (e[3][0] & 0xF0 == 0x90 and e[3][2] == 0), e[2]))
    return [(int(ms(tick)), message) for tick, _, _, message in found]


def main():
    data = open(sys.argv[1], "rb").read()
    for at, message in events(data):
        print(at, " ".join(f"{b:02x}" for b in message))


if __name__ == "__main__":
    main()
