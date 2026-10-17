import unittest


MANUFACTURER = 0x7D
SIGNATURE = (0x47, 0x4C, 0x4D)
VERSION = 0x01

GET_STATUS = 0x00
SET_SYNTH = 0x01
SET_ROM_SET = 0x02
SET_SOUNDFONT = 0x03
SET_USB_OUT = 0x04
SET_PARAMETER = 0x05

PARAM_VOLUME = 0x00
PARAM_EFFECTS = 0x01
PARAM_MIDI_CHANNELS = 0x02
PARAM_USB_IN_THRU = 0x03

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

    def test_usb_out_bridge_exchange(self):
        self.assertEqual(
            request(0x12, SET_USB_OUT, (0x02,)),
            bytes.fromhex("F0 7D 47 4C 4D 01 12 04 02 F7"),
        )
        self.assertEqual(
            response(0x12, ACCEPTED, SET_USB_OUT),
            bytes.fromhex("F0 7D 47 4C 4D 01 12 40 04 F7"),
        )
        self.assertEqual(
            response(0x12, READY, SET_USB_OUT, (0x02, 0x01)),
            bytes.fromhex("F0 7D 47 4C 4D 01 12 41 04 02 01 F7"),
        )

    def test_usb_out_query(self):
        self.assertEqual(
            request(0x13, SET_USB_OUT, (0x7F,)),
            bytes.fromhex("F0 7D 47 4C 4D 01 13 04 7F F7"),
        )

    def test_capabilities_with_usb_out(self):
        payload = (0x1F, 0x00, 0x03, 0x01, 0x02, 0x7F, 0x03)
        self.assertEqual(
            response(0x12, STATUS, GET_STATUS, payload),
            bytes.fromhex("F0 7D 47 4C 4D 01 12 43 00 1F 00 03 01 02 7F 03 F7"),
        )

    def test_set_parameter_volume_exchange(self):
        self.assertEqual(
            request(0x21, SET_PARAMETER, (PARAM_VOLUME, 80)),
            bytes.fromhex("F0 7D 47 4C 4D 01 21 05 00 50 F7"),
        )
        self.assertEqual(
            response(0x21, ACCEPTED, SET_PARAMETER),
            bytes.fromhex("F0 7D 47 4C 4D 01 21 40 05 F7"),
        )
        self.assertEqual(
            response(0x21, READY, SET_PARAMETER, (PARAM_VOLUME, 80)),
            bytes.fromhex("F0 7D 47 4C 4D 01 21 41 05 00 50 F7"),
        )

    def test_set_parameter_query(self):
        self.assertEqual(
            request(0x22, SET_PARAMETER, (PARAM_MIDI_CHANNELS, 0x7F)),
            bytes.fromhex("F0 7D 47 4C 4D 01 22 05 02 7F F7"),
        )

    def test_capabilities_with_set_parameter(self):
        payload = (0x3F, 0x00, 0x03, 0x01, 0x02, 0x7F, 0x03)
        self.assertEqual(
            response(0x12, STATUS, GET_STATUS, payload),
            bytes.fromhex("F0 7D 47 4C 4D 01 12 43 00 3F 00 03 01 02 7F 03 F7"),
        )

    def test_rejects_non_seven_bit_payload(self):
        with self.assertRaises(ValueError):
            request(0x12, SET_SYNTH, (0x80,))


def roland_dt1(address_and_data):
    """What CMT32Synth::QueueRolandWrite sends after an MT-32 reset."""
    checksum = (128 - (sum(address_and_data) & 0x7F)) & 0x7F
    return bytes([0xF0, 0x41, 0x10, 0x16, 0x12, *address_and_data, checksum, 0xF7])


class MT32KeptSettings(unittest.TestCase):
    def test_alternate_channels_after_reset(self):
        self.assertEqual(
            roland_dt1((0x10, 0x00, 0x0D, 0, 1, 2, 3, 4, 5, 6, 7, 9)),
            bytes.fromhex("F0 41 10 16 12 10 00 0D 00 01 02 03 04 05 06 07 09 3E F7"),
        )

    def test_master_volume_after_reset(self):
        self.assertEqual(
            roland_dt1((0x10, 0x00, 0x16, 80)),
            bytes.fromhex("F0 41 10 16 12 10 00 16 50 0A F7"),
        )


if __name__ == "__main__":
    unittest.main()
