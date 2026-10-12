#!/usr/bin/env python3
"""Reference peer for the gateway's private CAN FD bring-up protocol.

Offline fixtures work on any Python 3.10+ host. Live traffic uses Linux SocketCAN;
it requires an FD-capable adapter configured for 500 kbit/s / 2 Mbit/s.
"""
import argparse
import errno
import json
import secrets
import select
import socket
import struct
import sys
import time
import zlib

BASE = 0x1A500000
EFF = 0x80000000
BRS = 1
ANNOUNCE, ASSIGN, INPUT, HEARTBEAT = 1, 2, 3, 4
LENGTHS = {ANNOUNCE: 64, ASSIGN: 48, INPUT: 64, HEARTBEAT: 48}
FD_FRAME = struct.Struct("=IBBBB64s")


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


def frame(kind, unit, boot, *, node=0, direct=False, buttons=0, usages=(),
          neutrals=(), revision=1, lease=0, epoch=0, sequence=0, report=b""):
    if kind not in LENGTHS or len(unit) != 16 or not any(unit) or unit == b"\xff" * 16:
        raise ValueError("invalid message type or identity")
    if not 0 < boot <= 0xFFFFFFFF:
        raise ValueError("boot session must be nonzero uint32")
    if (kind == ANNOUNCE and node != 0) or (kind != ANNOUNCE and not 1 <= node <= 10):
        raise ValueError("invalid node for message type")
    if kind == ANNOUNCE:
        if not 0 <= buttons <= 64 or not 0 <= len(usages) <= 8 or not (buttons or usages):
            raise ValueError("profile needs 1..64 buttons and/or 1..8 axes")
        if len(neutrals) != len(usages) or len(set(usages)) != len(usages):
            raise ValueError("axis usages must be unique and match the neutrals")
        if any(not 0x30 <= u <= 0x38 for u in usages) or any(not 0 <= n <= 65535 for n in neutrals):
            raise ValueError("invalid axis usage or neutral")
        if not 1 <= revision <= 65535:
            raise ValueError("profile revision must be nonzero uint16")
    elif not 0 < lease <= 0xFFFFFFFF:
        raise ValueError("lease must be nonzero uint32")
    if direct and kind in (ASSIGN, INPUT):
        raise ValueError("direct USB ownership uses ANNOUNCE/HEARTBEAT, not INPUT")
    if kind == ASSIGN and not 0 < epoch <= 0xFFFFFFFF:
        raise ValueError("gateway epoch must be nonzero uint32")
    if not 0 <= sequence <= 0xFFFFFFFF or len(report) > 24:
        raise ValueError("invalid sequence or report length")
    data = bytearray(LENGTHS[kind])
    data[:8] = bytes((0x4F, 0x47, 1, kind, node, int(direct), 0, 0))
    data[8:24] = unit
    struct.pack_into("<I", data, 24, boot)
    if kind == ANNOUNCE:
        struct.pack_into("<BBH", data, 28, buttons, len(usages), revision)
        data[32:32 + len(usages)] = bytes(usages)
        for i, neutral in enumerate(neutrals):
            struct.pack_into("<H", data, 40 + i * 2, neutral)
    else:
        struct.pack_into("<II", data, 28, lease, epoch if kind == ASSIGN else sequence)
        if kind == INPUT:
            data[36:36 + len(report)] = report
    struct.pack_into("<I", data, len(data) - 4, zlib.crc32(data[:-4]))
    return BASE | kind << 8 | node, bytes(data)


def decode_assignment(can_id, data):
    if len(data) != 48 or data[:4] != bytes((0x4F, 0x47, 1, ASSIGN)):
        raise ValueError("invalid assignment framing")
    node = data[4]
    if can_id != BASE | ASSIGN << 8 | node or not 1 <= node <= 10:
        raise ValueError("invalid assignment CAN ID")
    if data[5] or any(data[6:8]) or any(data[36:44]):
        raise ValueError("invalid assignment reserved bytes")
    if zlib.crc32(data[:-4]) != struct.unpack_from("<I", data, 44)[0]:
        raise ValueError("assignment CRC mismatch")
    boot, lease, epoch = struct.unpack_from("<III", data, 24)
    if not all((boot, lease, epoch)) or not any(data[8:24]) or data[8:24] == b"\xff" * 16:
        raise ValueError("invalid assignment session")
    return data[8:24], boot, node, lease, epoch


def reference_vectors():
    unit = identity("00112233445566778899aabbccddeeff")
    common = dict(node=1, lease=0x11223344)
    samples = {
        "announce": frame(ANNOUNCE, unit, 0x10203040, buttons=3,
                          usages=(0x30, 0x31), neutrals=(0, 32768)),
        "assign": frame(ASSIGN, unit, 0x10203040, epoch=0x55667788, **common),
        "input": frame(INPUT, unit, 0x10203040, sequence=9,
                       report=bytes.fromhex("053412ffff"), **common),
        "heartbeat": frame(HEARTBEAT, unit, 0x10203040, sequence=10, **common),
    }
    return {name: {"id": f"0x{can_id:08x}", "data": data.hex()}
            for name, (can_id, data) in samples.items()}


def send_frame(bus, value):
    """Return false for temporary SocketCAN transmit backpressure."""
    can_id, data = value
    packet = FD_FRAME.pack(can_id | EFF, len(data), BRS, 0, 0, data)
    try:
        bus.send(packet)
    except OSError as error:
        if error.errno in (errno.EAGAIN, errno.EWOULDBLOCK, errno.ENOBUFS):
            return False
        raise
    return True


def run_peer(args):
    required = ("AF_CAN", "CAN_RAW", "SOL_CAN_RAW", "CAN_RAW_FD_FRAMES")
    if not all(hasattr(socket, key) for key in required):
        raise ValueError("live mode needs Linux SocketCAN; --vectors works on this host")
    unit = identity(args.identity)
    if not 0 <= args.buttons <= 64 or not 0 <= args.axes <= 8 or not (args.buttons or args.axes):
        raise ValueError("choose 0..64 buttons and 0..8 axes, with at least one control")
    usages = tuple(range(0x30, 0x30 + args.axes))
    axes = tuple(int(value, 0) for value in args.axis_values.split(",")) if args.axis_values else (0,) * args.axes
    if len(axes) != args.axes or any(not 0 <= value <= 65535 for value in axes):
        raise ValueError("--axis-values must contain one uint16 value per axis")
    neutrals = (0,) * args.axes
    if not 0 <= args.buttons_mask < 1 << args.buttons:
        raise ValueError("--buttons-mask contains a button outside the declared profile")
    report = args.buttons_mask.to_bytes((args.buttons + 7) // 8, "little")
    report += b"".join(struct.pack("<H", value) for value in axes)
    boot = secrets.randbelow(0xFFFFFFFF) + 1
    announcement = frame(ANNOUNCE, unit, boot, buttons=args.buttons, usages=usages,
                         neutrals=neutrals, direct=args.direct_usb)
    with socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as bus:
        bus.setsockopt(socket.SOL_CAN_RAW, socket.CAN_RAW_FD_FRAMES, 1)
        bus.bind((args.interface,))
        bus.setblocking(False)
        node = lease = epoch = sequence = 0
        assignment_key = None
        next_announce = next_input = 0.0
        deadline = time.monotonic() + args.duration if args.duration else float("inf")
        print(f"Announcing {unit.hex()} boot={boot:08x}; neutral by default. Ctrl-C stops.", flush=True)

        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_announce:
                next_announce = now + 0.5
                if not send_frame(bus, announcement):
                    node = lease = epoch = 0
                    next_input = next_announce
            if node and now >= next_input:
                sequence = (sequence + 1) & 0xFFFFFFFF
                next_input = now + 0.01
                if not send_frame(bus, frame(HEARTBEAT if args.direct_usb else INPUT, unit, boot,
                                            node=node, lease=lease, sequence=sequence,
                                            report=report, direct=args.direct_usb)):
                    node = lease = epoch = 0
                    next_input = next_announce = now + 0.5
            readable, _, _ = select.select([bus], [], [], 0.002)
            if not readable:
                continue
            packet = bus.recv(FD_FRAME.size)
            if len(packet) != FD_FRAME.size:
                continue
            can_id, length, flags, _, _, padded = FD_FRAME.unpack(packet)
            if can_id & 0x60000000 or not can_id & EFF or not flags & BRS or length > 64:
                continue
            try:
                remote, remote_boot, new_node, new_lease, new_epoch = decode_assignment(can_id & 0x1FFFFFFF, padded[:length])
            except ValueError:
                continue
            if remote != unit or remote_boot != boot:
                continue
            if (new_node, new_lease, new_epoch) != (node, lease, epoch):
                new_key = (new_node, new_lease, new_epoch)
                if new_key != assignment_key:
                    sequence = 0
                assignment_key = new_key
                node, lease, epoch = new_key
                print(f"Assigned node={node} lease={lease:08x} gateway_epoch={epoch:08x}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vectors", action="store_true", help="print reference frames as JSON; no hardware")
    parser.add_argument("--interface", default="can0")
    parser.add_argument("--identity", default="00112233445566778899aabbccddeeff")
    parser.add_argument("--buttons", type=int, default=3)
    parser.add_argument("--axes", type=int, default=2)
    parser.add_argument("--axis-values", default="", help="comma-separated uint16 inputs; defaults to zero")
    parser.add_argument("--buttons-mask", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--direct-usb", action="store_true", help="claim direct USB ownership; routed reports neutralize")
    parser.add_argument("--duration", type=float, default=0, help="seconds; zero means until Ctrl-C")
    args = parser.parse_args()
    try:
        if args.duration < 0:
            raise ValueError("duration cannot be negative")
        if args.vectors:
            print(json.dumps(reference_vectors(), indent=2))
        else:
            run_peer(args)
    except (ValueError, OSError) as error:
        parser.exit(2, f"{error}\n")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
