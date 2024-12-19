package com.tkr.engine;

import com.tkr.types.WireTypes;
import com.tkr.types.WireTypes.*;
import com.tkr.util.BoundsUtil;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/** Normalizes batch records: sort, dedupe, clamp qty, remap offsets. */
public final class BatchNormalizer {

    public static final class BatchNormalizeConfig {
        public boolean sortByRecordId = true;
        public boolean dedupeRecordIds = true;
        public boolean clampZeroQty = true;
    }

    public static final class BatchNormalizeResult {
        public Status status = Status.OK;
        public int recordsRemoved;
        public int recordsAdjusted;
    }

    private final BatchNormalizeConfig config;

    public BatchNormalizer() { this(new BatchNormalizeConfig()); }
    public BatchNormalizer(BatchNormalizeConfig config) {
        this.config = config != null ? config : new BatchNormalizeConfig();
    }

    public BatchNormalizeResult normalizeBatchRecords(BatchWireFrame frame) {
        BatchNormalizeResult result = new BatchNormalizeResult();
        if (frame == null || frame.records == null) { result.status = Status.BOUNDS_ERROR; return result; }
        if (config.sortByRecordId) {
            frame.records.sort(Comparator.comparingInt(r -> r.recordId));
        }
        if (config.dedupeRecordIds) {
            List<WireBatchRecord> deduped = new ArrayList<>();
            int lastId = -1;
            for (WireBatchRecord rec : frame.records) {
                if (rec.recordId == lastId) { result.recordsRemoved++; continue; }
                deduped.add(rec);
                lastId = rec.recordId;
            }
            frame.records = deduped;
        }
        for (WireBatchRecord rec : frame.records) {
            if (config.clampZeroQty && rec.qtyMilli == 0
                    && (frame.header.flags & WireTypes.BATCH_FLAG_PARTIAL_FILL) == 0) {
                rec.qtyMilli = 1;
                result.recordsAdjusted++;
            }
            if (rec.priceTick < 0) { rec.priceTick = 0; result.recordsAdjusted++; }
        }
        remapPayloadOffsets(frame, result);
        return result;
    }

    private void remapPayloadOffsets(BatchWireFrame frame, BatchNormalizeResult result) {
        if (frame.payloadBlob == null) frame.payloadBlob = new byte[0];
        int cursor = 0;
        for (WireBatchRecord rec : frame.records) {
            if (rec.payloadLen > 0) {
                if (!BoundsUtil.sliceInBounds(rec.payloadOffset, rec.payloadLen, frame.payloadBlob.length)) {
                    result.status = Status.BOUNDS_ERROR;
                    return;
                }
                cursor = Math.max(cursor, rec.payloadOffset + rec.payloadLen);
            }
        }
        if (cursor != frame.payloadBlob.length && frame.records.stream().anyMatch(r -> r.payloadLen > 0)) {
            result.status = Status.BOUNDS_ERROR;
        }
    }

    public long sumQtyMilli(BatchWireFrame frame) {
        long sum = 0;
        for (WireBatchRecord rec : frame.records) sum += rec.qtyMilli;
        return sum;
    }
}
