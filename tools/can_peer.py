#!/usr/bin/env python3
"""Cyphal/CAN module peer using the firmware's generated DSDL and libcanard stack.

Python 3.10+ and the native osg_gateway_peer shared library are required.
Live mode uses Linux SocketCAN at 500 kbit/s arbitration / 2 Mbit/s data.
"""
import argparse
from collections import deque
import ctypes
import errno
import json
import os
from pathlib import Path
import secrets
import select
import socket
import struct
import time

EFF, BRS = 0x80000000, 1
ANNOUNCE, ASSIGN, INPUT, HEARTBEAT, INPUT_ACK = 1, 2, 3, 4, 5
FD_FRAME = struct.Struct("=IBBBB64s")
CLASSIC_FRAME = struct.Struct("=IB3x8s")


class Message(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in ("kind", "boot", "lease", "epoch", "sequence")] + [
        ("identity", ctypes.c_uint8 * 16), ("node", ctypes.c_uint8), ("source", ctypes.c_uint8),
        ("direct", ctypes.c_uint8), ("buttons", ctypes.c_uint8), ("axes", ctypes.c_uint8),
        ("hats", ctypes.c_uint8), ("revision", ctypes.c_uint16), ("usages", ctypes.c_uint8 * 8),
        ("neutrals", ctypes.c_uint16 * 8), ("report", ctypes.c_uint8 * 34)]


class Frame(ctypes.Structure):
    _fields_ = [("id", ctypes.c_uint32), ("length", ctypes.c_uint8), ("extended", ctypes.c_uint8),
                ("fd", ctypes.c_uint8), ("brs", ctypes.c_uint8), ("data", ctypes.c_uint8 * 64),
                ("deadline_ms", ctypes.c_uint32), ("deadline_valid", ctypes.c_uint8)]


def identity(text):
    if len(text) != 32:
        raise ValueError("identity must contain exactly 32 hexadecimal characters")
    try:
        value = bytes.fromhex(text)
    except ValueError as error:
        raise ValueError("identity must contain exactly 32 hexadecimal characters") from error
    if len(value) != 16 or not any(value) or value == b"\xff" * 16:
        raise ValueError("identity cannot be all zero or all ff")
    return value


def load_library(path=None):
    path = path or os.environ.get("OSG_CYPHAL_PEER_LIBRARY")
    if not path:
        root = Path(__file__).resolve().parents[1]
        directories = (root / ".build/native",)
        path = next((str(directory / name) for directory in directories
                     for name in ("libosg_gateway_peer.so", "libosg_gateway_peer.dylib", "osg_gateway_peer.dll")
                     if (directory / name).is_file()), None)
    if not path:
        raise ValueError("build the native osg_gateway_peer target and set OSG_CYPHAL_PEER_LIBRARY or --library")
    library = ctypes.CDLL(str(path))
    library.peer_message_size.restype = library.peer_frame_size.restype = ctypes.c_size_t
    if (library.peer_message_size(), library.peer_frame_size()) != (ctypes.sizeof(Message), ctypes.sizeof(Frame)):
        raise ValueError("native peer library ABI differs; rebuild the current source")
    library.peer_new.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32, ctypes.c_char_p]
    library.peer_new.restype = ctypes.c_void_p
    library.peer_delete.argtypes = [ctypes.c_void_p]
    library.peer_delete.restype = None
    library.peer_node.argtypes = [ctypes.c_void_p]
    library.peer_node.restype = ctypes.c_uint
    library.peer_tick.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    library.peer_tick.restype = None
    library.peer_reset.argtypes = [ctypes.c_void_p]
    library.peer_reset.restype = None
    library.peer_send.argtypes = [ctypes.c_void_p, ctypes.POINTER(Message), ctypes.c_uint32]
    library.peer_receive.argtypes = [ctypes.c_void_p, ctypes.POINTER(Frame), ctypes.c_uint32, ctypes.POINTER(Message)]
    library.peer_pop.argtypes = [ctypes.c_void_p, ctypes.POINTER(Frame)]
    return library


class Peer:
    def __init__(self, unit, boot, library=None, name="org.opensimgear.socketcan"):
        self.library = library or load_library()
        self.handle = self.library.peer_new((ctypes.c_uint8 * 16).from_buffer_copy(unit), boot, name.encode())
        if not self.handle:
            raise ValueError("invalid Cyphal node identity, boot session or name")

    def close(self):
        if self.handle:
            self.library.peer_delete(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    @property
    def node(self):
        return self.library.peer_node(self.handle)

    def tick(self, now):
        self.library.peer_tick(self.handle, now)

    def reset(self):
        self.library.peer_reset(self.handle)

    def send(self, value, now):
        return bool(self.library.peer_send(self.handle, ctypes.byref(value), now))

    def receive(self, value, now):
        result = Message()
        if self.library.peer_receive(self.handle, ctypes.byref(value), now, ctypes.byref(result)):
            return result
        return None

    def frames(self):
        while True:
            value = Frame()
            if not self.library.peer_pop(self.handle, ctypes.byref(value)):
                return
            yield value


def message(kind, unit, boot, *, node=0, direct=False, buttons=0, usages=(),
            neutrals=(), hats=0, revision=1, lease=0, epoch=0, sequence=0, report=b""):
    if kind not in (ANNOUNCE, ASSIGN, INPUT, HEARTBEAT, INPUT_ACK):
        raise ValueError("invalid message type")
    if len(unit) != 16 or not any(unit) or unit == b"\xff" * 16:
        raise ValueError("invalid identity")
    if not 0 < boot <= 0xFFFFFFFF:
        raise ValueError("boot session must be nonzero uint32")
    if (kind == ANNOUNCE and node != 0) or (kind != ANNOUNCE and not 1 <= node <= 10):
        raise ValueError("invalid application route for message type")
    if kind == ANNOUNCE:
        if not 0 <= buttons <= 128 or not 0 <= hats <= 4 or not 0 <= len(usages) <= 8 or not (buttons or hats or usages):
            raise ValueError("profile needs up to 128 buttons, 4 hats, 8 axes and at least one control")
        if len(neutrals) != len(usages) or len(set(usages)) != len(usages):
            raise ValueError("axis usages must be unique and match the neutrals")
        if any(not 0x30 <= u <= 0x38 for u in usages) or any(not 0 <= n <= 65535 for n in neutrals):
            raise ValueError("invalid axis usage or neutral")
        if not 1 <= revision <= 65535:
            raise ValueError("profile revision must be nonzero uint16")
    elif not 0 < lease <= 0xFFFFFFFF:
        raise ValueError("lease must be nonzero uint32")
    if direct and kind in (ASSIGN, INPUT, INPUT_ACK):
        raise ValueError("direct USB ownership uses ANNOUNCE/HEARTBEAT, not INPUT")
    if kind == ASSIGN and not 0 < epoch <= 0xFFFFFFFF:
        raise ValueError("gateway epoch must be nonzero uint32")
    if not 0 <= sequence <= 0xFFFFFFFF or len(report) > 34:
        raise ValueError("invalid sequence or report length")
    result = Message(kind=kind, boot=boot, node=node, direct=direct, buttons=buttons,
                     axes=len(usages), hats=hats, revision=revision, lease=lease, epoch=epoch, sequence=sequence)
    result.identity[:] = unit
    result.usages[:len(usages)] = usages
    result.neutrals[:len(neutrals)] = neutrals
    result.report[:len(report)] = report
    return result


def reference_vectors(library=None):
    unit = identity("00112233445566778899aabbccddeeff")
    common = dict(node=1, lease=0x11223344)
    samples = {
        "announce": message(ANNOUNCE, unit, 0x10203040, buttons=3, usages=(0x30, 0x31), neutrals=(0, 32768)),
        "assign": message(ASSIGN, unit, 0x10203040, epoch=0x55667788, **common),
        "input": message(INPUT, unit, 0x10203040, sequence=9, report=bytes.fromhex("053412ffff"), **common),
        "status": message(HEARTBEAT, unit, 0x10203040, sequence=10, **common),
        "input_ack": message(INPUT_ACK, unit, 0x10203040, sequence=9, report=bytes.fromhex("053412ffff"), **common),
        "maximum_announce": message(ANNOUNCE, unit, 0x10203040, buttons=128, hats=4,
                                    usages=tuple(range(0x30, 0x38)), neutrals=tuple(i * 9000 for i in range(8))),
        "maximum_input": message(INPUT, unit, 0x10203040, sequence=9,
                                 report=bytes(range(1, 17)) + bytes((0x70, 0x88)) + bytes(range(19, 35)), **common),
    }
    result = {}
    for name, value in samples.items():
        with Peer(unit, 0x10203040, library) as peer:
            if not peer.send(value, 0):
                raise ValueError("native Cyphal serialization rejected reference message")
            result[name] = [{"id": f"0x{frame.id:08x}", "length": frame.length,
                             "data": bytes(frame.data[:frame.length]).hex()} for frame in peer.frames()]
    return result


def send_frame(bus, value):
    """Return false for temporary SocketCAN transmit backpressure."""
    can_id = value.id | (EFF if value.extended else 0)
    data = bytes(value.data[:value.length])
    packet = (FD_FRAME.pack(can_id, value.length, BRS if value.brs else 0, 0, 0, data)
              if value.fd else CLASSIC_FRAME.pack(can_id, value.length, data))
    try:
        bus.send(packet)
    except OSError as error:
        if error.errno in (errno.EAGAIN, errno.EWOULDBLOCK, errno.ENOBUFS):
            return False
        raise
    return True


def receive_frame(packet):
    if len(packet) == FD_FRAME.size:
        can_id, length, flags, _, _, data = FD_FRAME.unpack(packet)
        fd, brs = True, bool(flags & BRS)
    elif len(packet) == CLASSIC_FRAME.size:
        can_id, length, data = CLASSIC_FRAME.unpack(packet)
        fd, brs = False, False
    else:
        return None
    if can_id & 0x60000000 or not can_id & EFF or length > (64 if fd else 8):
        return None
    result = Frame(id=can_id & 0x1FFFFFFF, length=length, extended=True, fd=fd, brs=brs)
    result.data[:length] = data[:length]
    return result


def run_peer(args):
    required = ("AF_CAN", "CAN_RAW", "SOL_CAN_RAW", "CAN_RAW_FD_FRAMES")
    if not all(hasattr(socket, key) for key in required):
        raise ValueError("live mode needs Linux SocketCAN; --vectors works with the native library on this host")
    unit = identity(args.identity)
    if not 0 <= args.buttons <= 128 or not 0 <= args.hats <= 4 or not 0 <= args.axes <= 8 or not (args.buttons or args.hats or args.axes):
        raise ValueError("choose 0..128 buttons, 0..4 hats, 0..8 axes, with at least one control")
    usages = tuple(range(0x30, 0x30 + args.axes))
    axes = tuple(int(value, 0) for value in args.axis_values.split(",")) if args.axis_values else (0,) * args.axes
    if len(axes) != args.axes or any(not 0 <= value <= 65535 for value in axes):
        raise ValueError("--axis-values must contain one uint16 value per axis")
    if not 0 <= args.buttons_mask < 1 << args.buttons:
        raise ValueError("--buttons-mask contains a button outside the declared profile")
    report = args.buttons_mask.to_bytes((args.buttons + 7) // 8, "little")
    hats = tuple(int(value, 0) for value in args.hat_values.split(",")) if args.hat_values else (8,) * args.hats
    if len(hats) != args.hats or any(not 0 <= value <= 8 for value in hats):
        raise ValueError("--hat-values must contain one direction 0..7 or neutral 8 per hat")
    packed_hats = bytearray((args.hats + 1) // 2)
    for index, value in enumerate(hats):
        packed_hats[index // 2] |= value << ((index % 2) * 4)
    report += packed_hats + b"".join(struct.pack("<H", value) for value in axes)
    boot = secrets.randbelow(0xFFFFFFFF) + 1
    announcement = message(ANNOUNCE, unit, boot, buttons=args.buttons, usages=usages,
                           neutrals=(0,) * args.axes, hats=args.hats, direct=args.direct_usb)
    with Peer(unit, boot, load_library(getattr(args, "library", None))) as peer, \
            socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as bus:
        bus.setsockopt(socket.SOL_CAN_RAW, socket.CAN_RAW_FD_FRAMES, 1)
        bus.bind((args.interface,))
        bus.setblocking(False)
        node = lease = epoch = sequence = 0
        gateway_source = None
        last_assignment = 0
        assignment_key = None
        next_announce = next_input = 0
        pending = deque()
        started = time.monotonic()
        deadline = started + args.duration if args.duration else float("inf")
        print(f"Cyphal node={peer.node}; announcing {unit.hex()} boot={boot:08x}. Ctrl-C stops.", flush=True)
        while time.monotonic() < deadline:
            now = int((time.monotonic() - started) * 1000)
            if node and ((now - last_assignment) & 0xFFFFFFFF) >= 1500:
                node = lease = epoch = 0
                gateway_source = None
            if now >= next_announce:
                if peer.send(announcement, now):
                    next_announce = now + 500
            if node and now >= next_input:
                next_sequence = (sequence + 1) & 0xFFFFFFFF
                outgoing = message(HEARTBEAT if args.direct_usb else INPUT, unit, boot, node=node, lease=lease,
                                   sequence=next_sequence, report=report, direct=args.direct_usb)
                if peer.send(outgoing, now):
                    sequence = next_sequence
                    next_input = now + 10
            source = peer.node
            peer.tick(now)
            if peer.node != source:
                peer.reset()
                pending.clear()
                node = lease = epoch = 0
                gateway_source = assignment_key = None
                next_announce = now
            # Keep SocketCAN backpressure at the head of the bounded native
            # queue. A retained frame is retried before popping another frame.
            if not pending:
                pending.extend(peer.frames())
            while pending:
                outgoing_frame = pending[0]
                if outgoing_frame.deadline_valid and ((now - outgoing_frame.deadline_ms) & 0xFFFFFFFF) < 0x80000000:
                    pending.popleft()
                elif send_frame(bus, outgoing_frame):
                    pending.popleft()
                else:
                    break
            readable, _, _ = select.select([bus], [], [], 0.002)
            if not readable:
                continue
            incoming = receive_frame(bus.recv(FD_FRAME.size))
            source = peer.node
            received = peer.receive(incoming, now) if incoming else None
            if peer.node != source:
                peer.reset()
                pending.clear()
                node = lease = epoch = 0
                gateway_source = assignment_key = None
                next_announce = now
                continue
            if received is None:
                continue
            if received.kind == INPUT_ACK:
                if args.show_acks and received.source == gateway_source and (received.boot, received.node, received.lease) == (boot, node, lease):
                    print(f"USB delivered sequence={received.sequence} report={bytes(received.report[:len(report)]).hex()}", flush=True)
                continue
            if received.kind != ASSIGN or bytes(received.identity) != unit or received.boot != boot:
                continue
            if gateway_source is not None and received.source != gateway_source:
                continue
            new_key = (received.source, received.node, received.lease, received.epoch)
            last_assignment = now
            if new_key != assignment_key:
                assignment_key = new_key
                sequence = 0
                print(f"Assigned route={received.node} lease={received.lease:08x} gateway_epoch={received.epoch:08x} gateway_node={received.source}", flush=True)
            gateway_source, node, lease, epoch = new_key


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", help="path to the built native osg_gateway_peer shared library")
    parser.add_argument("--vectors", action="store_true", help="print native Cyphal transfers as JSON; no hardware")
    parser.add_argument("--interface", default="can0")
    parser.add_argument("--identity", default="00112233445566778899aabbccddeeff")
    parser.add_argument("--buttons", type=int, default=3)
    parser.add_argument("--axes", type=int, default=2)
    parser.add_argument("--hats", type=int, default=0)
    parser.add_argument("--hat-values", default="", help="comma-separated directions 0..7 or neutral 8; defaults neutral")
    parser.add_argument("--show-acks", action="store_true", help="print successful host USB delivery ACKs")
    parser.add_argument("--axis-values", default="", help="comma-separated uint16 inputs; defaults to zero")
    parser.add_argument("--buttons-mask", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--direct-usb", action="store_true", help="claim direct USB ownership; routed reports neutralize")
    parser.add_argument("--duration", type=float, default=0, help="seconds; zero means until Ctrl-C")
    args = parser.parse_args()
    try:
        if args.duration < 0:
            raise ValueError("duration cannot be negative")
        if args.vectors:
            print(json.dumps(reference_vectors(load_library(args.library)), indent=2))
        else:
            run_peer(args)
    except (ValueError, OSError) as error:
        parser.exit(2, f"{error}\n")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
