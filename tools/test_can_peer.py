import argparse
import errno
import itertools
import json
import os
from pathlib import Path
import subprocess
import unittest
from unittest import mock

import can_peer


class ReferencePeerTest(unittest.TestCase):
    def test_matches_firmware_vectors(self):
        path = Path(__file__).resolve().parents[1] / "firmware/tests/can-fd-v2-vectors.json"
        expected = json.loads(path.read_text())
        executable = os.environ.get("OSG_GATEWAY_TEST")
        if executable:
            fresh = json.loads(subprocess.check_output([executable, "--vectors"], text=True))
            self.assertEqual(fresh, expected, "C codec vectors differ from the stored fixtures")
        for name, frame in can_peer.reference_vectors().items():
            with self.subTest(message=name):
                self.assertEqual(int(frame["id"], 16), int(expected[name]["id"], 16))
                self.assertEqual(frame["data"], expected[name]["data"])
                self.assertEqual(len(bytes.fromhex(frame["data"])), expected[name]["length"])

    def test_assignment_is_bound_to_identity_and_session(self):
        encoded = can_peer.reference_vectors()["assign"]
        raw = bytes.fromhex(encoded["data"])
        self.assertEqual(can_peer.decode_assignment(int(encoded["id"], 16), raw),
                         (bytes.fromhex("00112233445566778899aabbccddeeff"),
                          0x10203040, 1, 0x11223344, 0x55667788))
        corrupt = bytearray(raw)
        corrupt[28] ^= 1
        with self.assertRaisesRegex(ValueError, "CRC"):
            can_peer.decode_assignment(int(encoded["id"], 16), corrupt)
        with self.assertRaisesRegex(ValueError, "CAN ID"):
            can_peer.decode_assignment(int(encoded["id"], 16) + 1, raw)

    def test_reserved_identity_is_rejected(self):
        for value in ("0" * 32, "f" * 32, "ab " * 10 + "ab", "12" * 15):
            with self.subTest(identity=value), self.assertRaises(ValueError):
                can_peer.identity(value)

    def test_direct_usb_uses_heartbeat(self):
        unit = can_peer.identity("00112233445566778899aabbccddeeff")
        with self.assertRaisesRegex(ValueError, "ownership"):
            can_peer.frame(can_peer.INPUT, unit, 1, node=1, lease=1, direct=True)
        _, raw = can_peer.frame(can_peer.HEARTBEAT, unit, 1, node=1, lease=1, direct=True)
        self.assertEqual(raw[5], 1)

    def test_ack_echoes_compact_delivered_input(self):
        values = can_peer.reference_vectors()
        raw = bytes.fromhex(values["input_ack"]["data"])
        boot, node, lease, sequence, report = can_peer.decode_ack(int(values["input_ack"]["id"], 16), raw)
        self.assertEqual((boot, node, lease, sequence), (0x10203040, 1, 0x11223344, 9))
        self.assertEqual(report, bytes.fromhex("053412ffff") + bytes(29))
        corrupt = bytearray(raw)
        corrupt[54] = 1
        import zlib
        corrupt[60:] = zlib.crc32(corrupt[:-4]).to_bytes(4, "little")
        with self.assertRaisesRegex(ValueError, "reserved"):
            can_peer.decode_ack(int(values["input_ack"]["id"], 16), corrupt)

    def test_cockpit_snapshots_use_matching_observed_profile(self):
        executable = os.environ.get("OSG_GATEWAY_TEST")
        if not executable:
            self.skipTest("set OSG_GATEWAY_TEST for fresh C snapshot fixtures")
        responses = json.loads(subprocess.check_output([executable, "--cockpit-vectors"], text=True))
        for name, response in responses.items():
            state = response["state"]
            self.assertEqual(state["protocol"], 2)
            for module in state["modules"]:
                profile = module["profile"]
                report = bytes.fromhex(module["report"])
                self.assertEqual(len(report), (profile["buttons"] + 7) // 8 + (profile["hats"] + 1) // 2 + 2 * profile["axes"], name)
                if not (state["running"] and module["online"] and module["profileMatch"] and module["hasInput"] and not module["directUsb"]):
                    self.assertEqual(int.from_bytes(report[:(profile["buttons"] + 7) // 8], "little"), 0, name)
                    offset = (profile["buttons"] + 7) // 8
                    for hat in range(profile["hats"]):
                        self.assertEqual((report[offset + hat // 2] >> (4 * (hat % 2))) & 15, 8, name)
        self.assertFalse(responses["profile_changed"]["state"]["modules"][0]["pendingProfileMatch"])
        self.assertTrue(responses["profile_updated_pending"]["state"]["modules"][0]["pendingProfileMatch"])
        self.assertFalse(responses["profile_updated_pending"]["state"]["modules"][0]["profileMatch"])
        self.assertEqual(responses["stale_epoch"]["error"], "stale_epoch")
        self.assertFalse(responses["offline"]["state"]["modules"][0]["online"])

    def test_live_peer_recovers_from_transmit_backpressure(self):
        args = argparse.Namespace(identity="00112233445566778899aabbccddeeff",
                                  buttons=3, axes=2, hats=0, hat_values="", show_acks=False, axis_values="", buttons_mask=0,
                                  direct_usb=False, interface="can0", duration=1.5)
        unit = can_peer.identity(args.identity)
        can_id, data = can_peer.frame(can_peer.ASSIGN, unit, 1, node=1, lease=2, epoch=3)
        bus = mock.MagicMock()
        bus.recv.return_value = can_peer.FD_FRAME.pack(can_id | can_peer.EFF, 48,
                                                      can_peer.BRS, 0, 0, data)
        input_sequences = []
        announcements = []

        def send(packet):
            _, _, _, _, _, payload = can_peer.FD_FRAME.unpack(packet)
            if payload[3] == can_peer.ANNOUNCE:
                announcements.append(packet)
                if len(announcements) == 1:
                    raise BlockingIOError(errno.EAGAIN, "temporary transmit backpressure")
            elif payload[3] == can_peer.INPUT:
                input_sequences.append(int.from_bytes(payload[16:20], "little"))
                if len(input_sequences) == 1:
                    raise OSError(errno.ENOBUFS, "CAN queue full")
            return len(packet)

        bus.send.side_effect = send
        constants = dict(AF_CAN=29, CAN_RAW=1, SOL_CAN_RAW=101, CAN_RAW_FD_FRAMES=5)
        ticks = itertools.count()
        with mock.patch.multiple(can_peer.socket, create=True, **constants), \
             mock.patch.object(can_peer.socket, "socket") as factory, \
             mock.patch.object(can_peer.secrets, "randbelow", return_value=0), \
             mock.patch.object(can_peer.time, "monotonic", side_effect=lambda: next(ticks) * 0.01), \
             mock.patch.object(can_peer.select, "select", return_value=([bus], [], [])), \
             mock.patch("builtins.print"):
            factory.return_value.__enter__.return_value = bus
            can_peer.run_peer(args)
        self.assertGreaterEqual(len(announcements), 2)
        self.assertGreaterEqual(len(input_sequences), 2)
        self.assertEqual(input_sequences, list(range(1, len(input_sequences) + 1)))


if __name__ == "__main__":
    unittest.main()
