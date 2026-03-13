# TKR Wire Format Specification

This document describes the binary wire formats used by `tkr-allocation-hub`.

## Magic Values

| Magic | ASCII | Purpose |
|-------|-------|---------|
| `0x544B5231` | TKR1 | Batch wire frame |
| `0x544B5232` | TKR2 | Envelope wire frame |
| `0x544B5233` | TKR3 | Session wire frame |

Wire version: **7**

## TKR1 Batch Frame

### Header (32 bytes, little-endian)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic (`0x544B5231`) |
| 4 | 2 | version |
| 6 | 2 | header_bytes |
| 8 | 4 | record_count |
| 12 | 4 | flags |
| 16 | 4 | desk_id |
| 20 | 4 | trade_date_yyyymmdd |
| 24 | 4 | payload_digest |

### Batch Flags

| Flag | Value | Meaning |
|------|-------|---------|
| `kBatchFlagProRata` | `1 << 0` | Use pro-rata weight allocation |
| `kBatchFlagMarginCheck` | `1 << 1` | Enforce margin check |
| `kBatchFlagComplianceHold` | `1 << 2` | Hold on compliance reject |
| `kBatchFlagDeferredDigest` | `1 << 3` | Enable deferred digest pipeline |
| `kBatchFlagPartialFill` | `1 << 4` | Partial fill (short locate) |
| `kBatchFlagCrossDesk` | `1 << 5` | Cross-desk allocation |

### Record (32 bytes each)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | record_id |
| 4 | 4 | account_id |
| 8 | 4 | symbol_id |
| 12 | 4 | qty_milli |
| 16 | 4 | price_tick |
| 20 | 4 | payload_offset |
| 24 | 4 | payload_len |
| 28 | 4 | flags |

Payload blob follows all records. Quantities are in milli-units (1000 = 1 share).

## TKR2 Envelope Frame

### Header (24 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic (`0x544B5232`) |
| 4 | 2 | version |
| 6 | 2 | channel_count |
| 8 | 4 | flags |
| 12 | 4 | ingress_seq |
| 16 | 4 | parent_batch_id |
| 20 | 4 | seal_digest |

### Channel (20 bytes each)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | channel_id |
| 4 | 4 | kind |
| 8 | 4 | payload_offset |
| 12 | 4 | payload_len |
| 16 | 4 | route_hint |

## TKR3 Session Frame

### Header (24 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic (`0x544B5233`) |
| 4 | 2 | version |
| 6 | 2 | leg_count |
| 8 | 4 | session_id |
| 12 | 4 | checkpoint_seq |
| 16 | 4 | flags |
| 20 | 4 | merge_digest |

### Leg (28 bytes each)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | leg_id |
| 4 | 4 | cl_ord_id |
| 8 | 4 | alloc_account |
| 12 | 4 | qty_milli |
| 16 | 4 | ref_offset |
| 20 | 4 | ref_len |
| 24 | 4 | flags |

Reference blob follows all legs.

## FIX 4.4

FIX messages use SOH (`0x01`) delimiters. Standard tag=value pairs. Checksum tag 10 validated on parse. Session state tracked across messages (sequence numbers, logon, allocation legs).

## SWIFT MT940

Block 4 tagged fields:

| Tag | Content |
|-----|---------|
| `:20:` | Transaction reference |
| `:25:` | Account identification |
| `:28C:` | Statement number |
| `:60F:` / `:60M:` | Opening balance |
| `:61:` | Statement line |
| `:86:` | Information to account owner |
| `:62F:` / `:62M:` | Closing balance |

Amounts parsed as comma-decimal with debit/credit indicator.

## Digest Algorithm

Payload and merge digests use FNV-1a 32-bit:

```
hash = 2166136261
for each byte b:
  hash = (hash ^ b) * 16777619
```

## Limits

| Constant | Value |
|----------|-------|
| `kMaxBatchRecords` | 4096 |
| `kMaxEnvelopeChannels` | 256 |
| `kMaxSessionLegs` | 512 |
| `kMaxDeferredSlots` | 1024 |
