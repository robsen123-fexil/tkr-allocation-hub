package com.tkr.wire;

import com.tkr.types.WireTypes;
import com.tkr.types.WireTypes.*;
import com.tkr.util.BoundsUtil;
import com.tkr.util.DigestUtil;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/** TKR1 batch wire codec - encode/decode binary batch frames with deferred slots. */
public final class BatchWireCodec {

    public static final int BATCH_HEADER_SIZE = 32;
    public static final int BATCH_RECORD_SIZE = 32;

    public static final class BatchCodecConfig {
        public boolean validateMarginFlags = true;
        public boolean verifyPayloadDigest = true;
        public int maxRecords = WireTypes.MAX_BATCH_RECORDS;
    }

    public static final class BatchDecodeResult {
        public Status status = Status.OK;
        public BatchWireFrame frame = new BatchWireFrame();
        public int bytesConsumed;
    }

    public static final class BatchEncodeResult {
        public Status status = Status.OK;
        public byte[] data = new byte[0];
    }

    private final BatchCodecConfig config;
    private int nextSlotId = 1;

    public BatchWireCodec() { this(new BatchCodecConfig()); }
    public BatchWireCodec(BatchCodecConfig config) {
        this.config = config != null ? config : new BatchCodecConfig();
    }

    public BatchDecodeResult decodeBatch(byte[] data, int offset, int length) {
        BatchDecodeResult result = new BatchDecodeResult();
        if (data == null || length < BATCH_HEADER_SIZE) {
            result.status = Status.TRUNCATED;
            return result;
        }
        WireBatchHeader header = new WireBatchHeader();
        readHeader(data, offset, header);
        Status vh = validateHeader(header);
        if (vh != Status.OK) { result.status = vh; return result; }
        result.frame.header = header;
        int recordBase = offset + BATCH_HEADER_SIZE;
        List<WireBatchRecord> records = new ArrayList<>();
        Status rr = readRecords(data, offset + length, recordBase, header.recordCount, records);
        if (rr != Status.OK) { result.status = rr; return result; }
        result.frame.records = records;
        int payloadStart = recordBase + header.recordCount * BATCH_RECORD_SIZE;
        byte[] blob = Arrays.copyOfRange(data, payloadStart, offset + length);
        result.frame.payloadBlob = blob;
        for (WireBatchRecord rec : records) {
            Status rb = validateRecordBounds(rec, blob.length);
            if (rb != Status.OK) { result.status = rb; return result; }
        }
        if (config.verifyPayloadDigest && header.payloadDigest != 0) {
            if (DigestUtil.fnv1a32(blob) != header.payloadDigest) {
                result.status = Status.BOUNDS_ERROR;
                return result;
            }
        }
        if ((header.flags & WireTypes.BATCH_FLAG_DEFERRED_DIGEST) != 0) {
            buildDeferredSlots(result.frame);
        }
        result.bytesConsumed = length;
        return result;
    }

    public BatchDecodeResult decodeBatch(byte[] data) {
        return decodeBatch(data, 0, data != null ? data.length : 0);
    }

    public BatchEncodeResult encodeBatch(BatchWireFrame frame) {
        BatchEncodeResult result = new BatchEncodeResult();
        if (frame == null || frame.records.size() > config.maxRecords) {
            result.status = Status.BOUNDS_ERROR;
            return result;
        }
        if (frame.payloadBlob == null) frame.payloadBlob = new byte[0];
        frame.header.magic = WireTypes.BATCH_MAGIC;
        frame.header.version = WireTypes.WIRE_VERSION;
        frame.header.headerBytes = BATCH_HEADER_SIZE;
        frame.header.recordCount = frame.records.size();
        frame.header.payloadDigest = DigestUtil.fnv1a32(frame.payloadBlob);
        int total = BATCH_HEADER_SIZE + frame.records.size() * BATCH_RECORD_SIZE + frame.payloadBlob.length;
        byte[] out = new byte[total];
        writeHeader(out, 0, frame.header);
        int cursor = BATCH_HEADER_SIZE;
        for (WireBatchRecord rec : frame.records) {
            writeRecord(out, cursor, rec);
            cursor += BATCH_RECORD_SIZE;
        }
        System.arraycopy(frame.payloadBlob, 0, out, cursor, frame.payloadBlob.length);
        result.data = out;
        return result;
    }

    private void readHeader(byte[] data, int offset, WireBatchHeader out) {
        out.magic = BoundsUtil.readU32Le(data, offset);
        out.version = BoundsUtil.readU16Le(data, offset + 4);
        out.headerBytes = BoundsUtil.readU16Le(data, offset + 6);
        out.recordCount = BoundsUtil.readU32Le(data, offset + 8);
        out.flags = BoundsUtil.readU32Le(data, offset + 12);
        out.deskId = BoundsUtil.readU32Le(data, offset + 16);
        out.tradeDateYyyymmdd = BoundsUtil.readU32Le(data, offset + 20);
        out.payloadDigest = BoundsUtil.readU32Le(data, offset + 24);
    }

    private void writeHeader(byte[] dst, int offset, WireBatchHeader h) {
        BoundsUtil.writeU32Le(dst, offset, h.magic);
        BoundsUtil.writeU16Le(dst, offset + 4, h.version);
        BoundsUtil.writeU16Le(dst, offset + 6, h.headerBytes);
        BoundsUtil.writeU32Le(dst, offset + 8, h.recordCount);
        BoundsUtil.writeU32Le(dst, offset + 12, h.flags);
        BoundsUtil.writeU32Le(dst, offset + 16, h.deskId);
        BoundsUtil.writeU32Le(dst, offset + 20, h.tradeDateYyyymmdd);
        BoundsUtil.writeU32Le(dst, offset + 24, h.payloadDigest);
    }

    private Status readRecords(byte[] data, int totalSize, int offset, int count, List<WireBatchRecord> out) {
        if (!BoundsUtil.sectionBodyInBounds(offset, (long) count * BATCH_RECORD_SIZE, totalSize)) {
            return Status.BOUNDS_ERROR;
        }
        Set<Integer> seen = new HashSet<>();
        for (int i = 0; i < count; i++) {
            int base = offset + i * BATCH_RECORD_SIZE;
            WireBatchRecord rec = new WireBatchRecord();
            rec.recordId = BoundsUtil.readU32Le(data, base);
            rec.accountId = BoundsUtil.readU32Le(data, base + 4);
            rec.symbolId = BoundsUtil.readU32Le(data, base + 8);
            rec.qtyMilli = BoundsUtil.readU32Le(data, base + 12);
            rec.priceTick = BoundsUtil.readU32Le(data, base + 16);
            rec.payloadOffset = BoundsUtil.readU32Le(data, base + 20);
            rec.payloadLen = BoundsUtil.readU32Le(data, base + 24);
            rec.flags = BoundsUtil.readU32Le(data, base + 28);
            if (!seen.add(rec.recordId)) return Status.DUPLICATE_KEY;
            out.add(rec);
        }
        return Status.OK;
    }

    private void writeRecord(byte[] dst, int offset, WireBatchRecord rec) {
        BoundsUtil.writeU32Le(dst, offset, rec.recordId);
        BoundsUtil.writeU32Le(dst, offset + 4, rec.accountId);
        BoundsUtil.writeU32Le(dst, offset + 8, rec.symbolId);
        BoundsUtil.writeU32Le(dst, offset + 12, rec.qtyMilli);
        BoundsUtil.writeU32Le(dst, offset + 16, rec.priceTick);
        BoundsUtil.writeU32Le(dst, offset + 20, rec.payloadOffset);
        BoundsUtil.writeU32Le(dst, offset + 24, rec.payloadLen);
        BoundsUtil.writeU32Le(dst, offset + 28, rec.flags);
    }

    private Status validateHeader(WireBatchHeader header) {
        if (header.magic != WireTypes.BATCH_MAGIC) return Status.INVALID_MAGIC;
        if (header.version != WireTypes.WIRE_VERSION) return Status.BOUNDS_ERROR;
        if (header.headerBytes < BATCH_HEADER_SIZE) return Status.TRUNCATED;
        if (header.recordCount > config.maxRecords) return Status.BOUNDS_ERROR;
        if (config.validateMarginFlags && (header.flags & WireTypes.BATCH_FLAG_MARGIN_CHECK) != 0
                && (header.flags & WireTypes.BATCH_FLAG_PRO_RATA) == 0) {
            return Status.MARGIN_BREACH;
        }
        return Status.OK;
    }

    private Status validateRecordBounds(WireBatchRecord rec, int payloadSize) {
        if (!BoundsUtil.sliceInBounds(rec.payloadOffset, rec.payloadLen, payloadSize)) {
            return Status.BOUNDS_ERROR;
        }
        if (rec.qtyMilli == 0 && (rec.flags & WireTypes.BATCH_FLAG_PARTIAL_FILL) == 0) {
            return Status.COMPLIANCE_REJECT;
        }
        return Status.OK;
    }

    private void buildDeferredSlots(BatchWireFrame frame) {
        frame.deferredSlots.clear();
        for (WireBatchRecord rec : frame.records) {
            if (rec.payloadLen == 0) continue;
            DeferredSlot slot = new DeferredSlot();
            slot.slotId = nextSlotId++;
            slot.recordId = rec.recordId;
            slot.payloadLen = rec.payloadLen;
            slot.payload = Arrays.copyOfRange(frame.payloadBlob, rec.payloadOffset,
                    rec.payloadOffset + rec.payloadLen);
            slot.active = true;
            frame.deferredSlots.add(slot);
        }
    }

    public int computeHeaderDigest(WireBatchHeader header) {
        byte[] buf = new byte[BATCH_HEADER_SIZE];
        WireBatchHeader copy = new WireBatchHeader();
        copy.magic = header.magic; copy.version = header.version;
        copy.headerBytes = header.headerBytes; copy.recordCount = header.recordCount;
        copy.flags = header.flags; copy.deskId = header.deskId;
        copy.tradeDateYyyymmdd = header.tradeDateYyyymmdd; copy.payloadDigest = 0;
        writeHeader(buf, 0, copy);
        return DigestUtil.fnv1a32(buf);
    }

    public long sumQtyMilli(BatchWireFrame frame) {
        long sum = 0;
        for (WireBatchRecord rec : frame.records) sum += rec.qtyMilli;
        return sum;
    }
}
