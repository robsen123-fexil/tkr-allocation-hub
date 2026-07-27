package com.tkr.wire;

import com.tkr.types.WireTypes;
import com.tkr.types.WireTypes.BatchWireFrame;
import com.tkr.types.WireTypes.Status;
import com.tkr.types.WireTypes.WireBatchHeader;
import com.tkr.types.WireTypes.WireBatchRecord;
import com.tkr.util.BoundsUtil;
import com.tkr.util.DigestUtil;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;

/**
 * Validates structural integrity of TKR wire frames before pipeline processing.
 */
public final class WireValidator {

    public static final class ValidationIssue {
        public String code;
        public String detail;
        public int recordIndex = -1;
    }

    public static final class ValidationResult {
        public Status status = Status.OK;
        public List<ValidationIssue> issues = new ArrayList<>();
        public int recordsChecked;
        public int payloadBytesChecked;
    }

    private final boolean strictDigest;
    private final int maxRecords;

    public WireValidator(boolean strictDigest, int maxRecords) {
        this.strictDigest = strictDigest;
        this.maxRecords = maxRecords;
    }

    public ValidationResult validateBatchFrame(BatchWireFrame frame) {
        ValidationResult result = new ValidationResult();
        if (frame == null) {
            result.status = Status.BOUNDS_ERROR;
            addIssue(result, "null_frame", "Batch frame is null");
            return result;
        }

        WireBatchHeader header = frame.header;
        if (header == null) {
            result.status = Status.INVALID_MAGIC;
            addIssue(result, "missing_header", "Batch header missing");
            return result;
        }

        if (header.magic != WireTypes.BATCH_MAGIC) {
            result.status = Status.INVALID_MAGIC;
            addIssue(result, "bad_magic", "Expected TKR1 magic");
            return result;
        }

        if (header.version != WireTypes.WIRE_VERSION) {
            result.status = Status.BOUNDS_ERROR;
            addIssue(result, "bad_version", "Unsupported wire version");
        }

        if (frame.records == null) {
            result.status = Status.TRUNCATED;
            addIssue(result, "missing_records", "Record table missing");
            return result;
        }

        if (frame.records.size() != header.recordCount) {
            result.status = Status.BOUNDS_ERROR;
            addIssue(result, "record_count_mismatch", "Header count differs from table");
        }

        if (frame.records.size() > maxRecords) {
            result.status = Status.BOUNDS_ERROR;
            addIssue(result, "too_many_records", "Record count exceeds limit");
        }

        byte[] payload = frame.payloadBlob != null ? frame.payloadBlob : new byte[0];
        result.payloadBytesChecked = payload.length;

        for (int i = 0; i < frame.records.size(); i++) {
            WireBatchRecord rec = frame.records.get(i);
            result.recordsChecked++;
            validateRecord(result, rec, payload, i);
        }

        if (strictDigest && header.payloadDigest != 0) {
            int computed = DigestUtil.fnv1a32(payload);
            if (computed != header.payloadDigest) {
                result.status = Status.BOUNDS_ERROR;
                addIssue(result, "digest_mismatch", "Payload digest mismatch");
            }
        }

        if (!result.issues.isEmpty() && result.status == Status.OK) {
            result.status = Status.BOUNDS_ERROR;
        }
        return result;
    }

    public ValidationResult validateBatchBytes(byte[] data) {
        ValidationResult result = new ValidationResult();
        if (data == null || data.length < 28) {
            result.status = Status.TRUNCATED;
            addIssue(result, "truncated", "Batch frame too short");
            return result;
        }

        ByteBuffer buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        int magic = buf.getInt(0);
        if (magic != WireTypes.BATCH_MAGIC) {
            result.status = Status.INVALID_MAGIC;
            addIssue(result, "bad_magic", "Not a TKR1 frame");
            return result;
        }

        int recordCount = buf.getInt(8);
        if (recordCount < 0 || recordCount > maxRecords) {
            result.status = Status.BOUNDS_ERROR;
            addIssue(result, "bad_record_count", "Invalid record count");
            return result;
        }

        int headerSize = 28;
        int tableBytes = recordCount * 32;
        if (!BoundsUtil.recordTableInBounds(headerSize, recordCount, 32, data.length)) {
            result.status = Status.TRUNCATED;
            addIssue(result, "truncated_table", "Record table exceeds frame");
            return result;
        }

        result.recordsChecked = recordCount;
        result.payloadBytesChecked = data.length - headerSize - tableBytes;
        return result;
    }

    public ValidationResult validateEnvelopeBytes(byte[] data) {
        ValidationResult result = new ValidationResult();
        if (data == null || data.length < 24) {
            result.status = Status.TRUNCATED;
            addIssue(result, "truncated", "Envelope too short");
            return result;
        }
        ByteBuffer buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        if (buf.getInt(0) != WireTypes.ENVELOPE_MAGIC) {
            result.status = Status.INVALID_MAGIC;
            addIssue(result, "bad_magic", "Not a TKR2 envelope");
            return result;
        }
        int channelCount = Short.toUnsignedInt(buf.getShort(6));
        if (channelCount > WireTypes.MAX_ENVELOPE_CHANNELS) {
            result.status = Status.BOUNDS_ERROR;
            addIssue(result, "too_many_channels", "Channel count exceeds max");
        }
        return result;
    }

    public ValidationResult validateSessionBytes(byte[] data) {
        ValidationResult result = new ValidationResult();
        if (data == null || data.length < 28) {
            result.status = Status.TRUNCATED;
            addIssue(result, "truncated", "Session frame too short");
            return result;
        }
        ByteBuffer buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        if (buf.getInt(0) != WireTypes.SESSION_MAGIC) {
            result.status = Status.INVALID_MAGIC;
            addIssue(result, "bad_magic", "Not a TKR3 session");
            return result;
        }
        int legCount = Short.toUnsignedInt(buf.getShort(6));
        if (legCount > WireTypes.MAX_SESSION_LEGS) {
            result.status = Status.BOUNDS_ERROR;
            addIssue(result, "too_many_legs", "Leg count exceeds max");
        }
        return result;
    }

    private void validateRecord(
            ValidationResult result, WireBatchRecord rec, byte[] payload, int index) {
        if (rec.qtyMilli == 0 && rec.payloadLen > 0) {
            addIssue(result, "zero_qty_payload", "Zero qty with payload", index);
        }
        if (rec.payloadLen > 0) {
            if (!BoundsUtil.sliceInBounds(rec.payloadOffset, rec.payloadLen, payload.length)) {
                result.status = Status.BOUNDS_ERROR;
                addIssue(result, "payload_oob", "Payload out of bounds", index);
            }
        }
        if (rec.priceTick < 0) {
            addIssue(result, "negative_price", "Negative price tick", index);
        }
        if ((rec.flags & WireTypes.BATCH_FLAG_MARGIN_CHECK) != 0
                && (rec.flags & WireTypes.BATCH_FLAG_PRO_RATA) == 0) {
            addIssue(result, "margin_without_prorata", "Margin flag without pro-rata", index);
        }
    }

    private static void addIssue(ValidationResult result, String code, String detail) {
        addIssue(result, code, detail, -1);
    }

    private static void addIssue(
            ValidationResult result, String code, String detail, int recordIndex) {
        ValidationIssue issue = new ValidationIssue();
        issue.code = code;
        issue.detail = detail;
        issue.recordIndex = recordIndex;
        result.issues.add(issue);
    }
}
