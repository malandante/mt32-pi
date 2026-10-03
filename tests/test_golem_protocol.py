import unittest


MANUFACTURER = 0x7D
SIGNATURE = (0x47, 0x4C, 0x4D)
VERSION = 0x01

GET_STATUS = 0x00
SET_SYNTH = 0x01
SET_ROM_SET = 0x02
SET_SOUNDFONT = 0x03

ACCEPTED = 0x40
READY = 0x41
ERROR = 0x42
STATUS = 0x43


def request(transaction, command, payload=()):
    data = [0xF0, MANUFACTURER, *SIGNATURE, VERSION, transaction, command, *payload, 0xF7]
    _validate(data)
    return bytes(data)


def response(transaction, response_type, command, payload=()):
    data = [
        0xF0,
        MANUFACTURER,
        *SIGNATURE,
        VERSION,
        transaction,
        response_type,
        command,
        *payload,
        0xF7,
    ]
    _validate(data)
    return bytes(data)


def _validate(frame):
    if frame[0] != 0xF0 or frame[-1] != 0xF7:
        raise ValueError("not a complete SysEx frame")
    if any(value & 0x80 for value in frame[1:-1]):
        raise ValueError("SysEx data bytes must be 7-bit")


class GolemProtocolVectors(unittest.TestCase):
    def test_status_request(self):
        self.assertEqual(
            request(0x12, GET_STATUS),
            bytes.fromhex("F0 7D 47 4C 4D 01 12 00 F7"),
        )

    def test_soundfont_511_request(self):
        self.assertEqual(
            request(0x12, SET_SOUNDFONT, (0x7F, 0x03)),
            bytes.fromhex("F0 7D 47 4C 4D 01 12 03 7F 03 F7"),
        )

    def test_accepted_and_ready_keep_transaction_and_command(self):
        self.assertEqual(
            response(0x12, ACCEPTED, SET_SYNTH),
            bytes.fromhex("F0 7D 47 4C 4D 01 12 40 01 F7"),
        )
        self.assertEqual(
            response(0x12, READY, SET_SYNTH, (0x01,)),
            bytes.fromhex("F0 7D 47 4C 4D 01 12 41 01 01 F7"),
        )

    def test_status_payload(self):
        payload = (0x0F, 0x00, 0x03, 0x01, 0x02, 0x7F, 0x03)
        self.assertEqual(
            response(0x12, STATUS, GET_STATUS, payload),
            bytes.fromhex("F0 7D 47 4C 4D 01 12 43 00 0F 00 03 01 02 7F 03 F7"),
        )

    def test_rejects_non_seven_bit_payload(self):
        with self.assertRaises(ValueError):
            request(0x12, SET_SYNTH, (0x80,))


if __name__ == "__main__":
    unittest.main()
