import errno
import json
import os
import subprocess
import unittest
from unittest import mock

import can_peer


class ReferencePeerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = can_peer.load_library()
        cls.unit = can_peer.identity("00112233445566778899aabbccddeeff")
        cls.gateway = can_peer.identity("112233445566778899aabbccddeeff00")

    def roundtrip(self, value):
        with can_peer.Peer(self.gateway, 0x10000, self.library) as sender, \
                can_peer.Peer(self.unit, 100, self.library) as receiver:
            self.assertNotEqual(sender.node, receiver.node)
            self.assertTrue(sender.send(value, 0))
            frames = list(sender.frames())
            self.assertEqual(len(frames), 1)
            received = receiver.receive(frames[0], 0)
            self.assertIsNotNone(received)
            self.assertEqual(received.source, sender.node)
            self.assertIsNone(receiver.receive(frames[0], 1), "Cyphal transfer-ID duplicate must be suppressed")
            return received, frames[0]

    def test_all_application_transfers_use_native_cyphal(self):
        vectors = can_peer.reference_vectors(self.library)
        self.assertEqual(set(vectors), {"announce", "assign", "input", "status", "input_ack", "maximum_announce", "maximum_input"})
        for name, frames in vectors.items():
            with self.subTest(message=name):
                self.assertEqual(len(frames), 1)
                frame = frames[0]
                raw = bytes.fromhex(frame["data"])
                self.assertEqual(len(raw), frame["length"])
                self.assertEqual(raw[-1] & 0xE0, 0xE0, "single-frame Cyphal tail")
                self.assertNotEqual(int(frame["id"], 16) & 0x1FFFF800, 0x1A500000)
                self.assertFalse(raw.startswith(b"OG\x02"), "private framing is retired")

    def test_assignment_preserves_identity_session_and_transport_source(self):
        value = can_peer.message(can_peer.ASSIGN, self.unit, 100, node=1, lease=0x10001, epoch=0x10000)
        received, frame = self.roundtrip(value)
        self.assertEqual((bytes(received.identity), received.boot, received.node, received.lease, received.epoch),
                         (self.unit, 100, 1, 0x10001, 0x10000))
        self.assertNotEqual(received.source, received.node)
        frame.extended = False
        with can_peer.Peer(self.unit, 100, self.library) as receiver:
            self.assertIsNone(receiver.receive(frame, 0))

    def test_reserved_identity_is_rejected(self):
        for value in ("0" * 32, "f" * 32, "ab " * 10 + "ab", "12" * 15):
            with self.subTest(identity=value), self.assertRaises(ValueError):
                can_peer.identity(value)

    def test_direct_usb_uses_status(self):
        with self.assertRaisesRegex(ValueError, "ownership"):
            can_peer.message(can_peer.INPUT, self.unit, 100, node=1, lease=1, direct=True)
        received, _ = self.roundtrip(can_peer.message(can_peer.HEARTBEAT, self.unit, 100,
                                                     node=1, lease=1, sequence=9, direct=True))
        self.assertEqual(received.direct, 1)

    def test_ack_echoes_compact_delivered_input(self):
        received, _ = self.roundtrip(can_peer.message(can_peer.INPUT_ACK, self.unit, 100, node=1,
                                                     lease=0x10001, sequence=9, report=bytes.fromhex("053412ffff")))
        self.assertEqual((received.boot, received.node, received.lease, received.sequence), (100, 1, 0x10001, 9))
        self.assertEqual(bytes(received.report), bytes.fromhex("053412ffff") + bytes(29))
        self.assertEqual(bytes(received.identity), bytes(16))

    def test_maximum_announcement_preserves_full_profile(self):
        received, _ = self.roundtrip(can_peer.message(can_peer.ANNOUNCE, self.unit, 100, buttons=128, hats=4,
                                                     usages=tuple(range(0x30, 0x38)), neutrals=tuple(i * 9000 for i in range(8))))
        self.assertEqual((received.buttons, received.axes, received.hats), (128, 8, 4))
        self.assertEqual(tuple(received.usages), tuple(range(0x30, 0x38)))
        self.assertEqual(tuple(received.neutrals), tuple(i * 9000 for i in range(8)))

    def test_socketcan_backpressure_retains_frame_for_retry(self):
        value = can_peer.message(can_peer.INPUT, self.unit, 100, node=1, lease=1, sequence=9)
        _, frame = self.roundtrip(value)
        bus = mock.Mock()
        bus.send.side_effect = [BlockingIOError(errno.EAGAIN, "temporary backpressure"), OSError(errno.ENOBUFS, "full"), 72]
        self.assertFalse(can_peer.send_frame(bus, frame))
        self.assertFalse(can_peer.send_frame(bus, frame))
        self.assertTrue(can_peer.send_frame(bus, frame))
        self.assertEqual(bus.send.call_args_list[0], bus.send.call_args_list[1])
        self.assertEqual(bus.send.call_args_list[1], bus.send.call_args_list[2])
        packet = bus.send.call_args.args[0]
        decoded = can_peer.receive_frame(packet)
        self.assertEqual((decoded.id, decoded.length, decoded.fd, decoded.brs),
                         (frame.id, frame.length, frame.fd, frame.brs))
        self.assertEqual(bytes(decoded.data), bytes(frame.data))

    def test_socketcan_accepts_classic_and_fd_without_brs(self):
        classic = can_peer.CLASSIC_FRAME.pack(0x123456 | can_peer.EFF, 8, bytes(range(8)))
        received = can_peer.receive_frame(classic)
        self.assertEqual((received.id, received.length, received.fd, received.brs), (0x123456, 8, 0, 0))
        fd = can_peer.FD_FRAME.pack(0x123456 | can_peer.EFF, 12, 0, 0, 0, bytes(range(12)))
        received = can_peer.receive_frame(fd)
        self.assertEqual((received.length, received.fd, received.brs), (12, 1, 0))
        self.assertIsNone(can_peer.receive_frame(can_peer.CLASSIC_FRAME.pack(0x123456, 8, bytes(8))))

    def test_reset_discards_queued_frames_and_retains_transport_counters(self):
        value = can_peer.message(can_peer.INPUT, self.unit, 100, node=1, lease=1, sequence=9)
        with can_peer.Peer(self.unit, 100, self.library) as peer:
            source = peer.node
            self.assertTrue(peer.send(value, 0))
            peer.reset()
            self.assertEqual(peer.node, source)
            self.assertEqual(list(peer.frames()), [])
            self.assertTrue(peer.send(value, 1))
            frames = list(peer.frames())
            self.assertEqual(len(frames), 1)
            self.assertEqual(frames[0].data[frames[0].length - 1] & 31, 1)

    def test_cockpit_snapshots_use_matching_observed_profile(self):
        executable = os.environ.get("OSG_GATEWAY_TEST")
        if not executable:
            self.skipTest("set OSG_GATEWAY_TEST for fresh C snapshot fixtures")
        responses = json.loads(subprocess.check_output([executable, "--cockpit-vectors"], text=True))
        for name, response in responses.items():
            state = response["state"]
            self.assertEqual((state["protocol"], state["canProtocol"]), (2, 3))
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


if __name__ == "__main__":
    unittest.main()
