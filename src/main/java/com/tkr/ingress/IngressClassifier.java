package com.tkr.ingress;

import com.tkr.types.WireTypes;
import com.tkr.types.WireTypes.IngressKind;
import com.tkr.types.WireTypes.Status;
import com.tkr.util.BoundsUtil;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/**
 * Classifies ingress byte streams into FIX, SWIFT, or TKR wire formats.
 * Ported from legacy-cpp ingress_classifier.cc.
 */
public final class IngressClassifier {

    public static final class IngressProbe {
        public IngressKind kind = IngressKind.UNKNOWN;
        public int magicOrTag;
        public int frameHintBytes;
        public float confidence;
    }

    public static final class IngressClassifyResult {
        public Status status = Status.OK;
        public IngressKind primaryKind = IngressKind.UNKNOWN;
        public boolean isMixedStream;
        public int leadingSkip;
        public List<IngressProbe> probes = new ArrayList<>();
    }

    public static final class IngressClassifierConfig {
        public boolean acceptMixed = true;
        public boolean preferBinaryMagic = true;
        public int minFixHeaderLen = 10;
    }

    private final IngressClassifierConfig config;

    public IngressClassifier() {
        this(new IngressClassifierConfig());
    }

    public IngressClassifier(IngressClassifierConfig config) {
        this.config = config != null ? config : new IngressClassifierConfig();
    }

    public IngressKind classify(byte[] data, int offset, int length) {
        return classifyBinary(data, offset, length).primaryKind;
    }

    public IngressKind classify(byte[] data) {
        return classify(data, 0, data != null ? data.length : 0);
    }

    public IngressClassifyResult classifyBinary(byte[] data, int offset, int length) {
        if (data == null || length == 0) {
            IngressClassifyResult result = new IngressClassifyResult();
            result.status = Status.TRUNCATED;
            return result;
        }
        String view = new String(data, offset, length, StandardCharsets.US_ASCII);
        return classify(view, data, offset, length);
    }

    public IngressClassifyResult classify(String bytes) {
        byte[] data = bytes.getBytes(StandardCharsets.US_ASCII);
        return classify(bytes, data, 0, data.length);
    }

    private IngressClassifyResult classify(String bytes, byte[] data, int offset, int length) {
        IngressClassifyResult result = new IngressClassifyResult();
        result.status = Status.OK;
        result.primaryKind = IngressKind.UNKNOWN;
        if (bytes.isEmpty()) {
            result.status = Status.TRUNCATED;
            return result;
        }
        int printable = countPrintablePrefix(data, offset, length);
        if (printable > 0 && printable < length) result.leadingSkip = printable;

        IngressProbe fixProbe = probeFix44(bytes);
        if (fixProbe.confidence > 0) result.probes.add(fixProbe);

        IngressProbe swiftProbe = probeSwift(bytes);
        if (swiftProbe.confidence > 0) result.probes.add(swiftProbe);

        IngressProbe batchProbe = probeBatchWire(data, offset, length);
        if (batchProbe.confidence > 0) result.probes.add(batchProbe);

        IngressProbe envelopeProbe = probeEnvelopeWire(data, offset, length);
        if (envelopeProbe.confidence > 0) result.probes.add(envelopeProbe);

        IngressProbe sessionProbe = probeSessionWire(data, offset, length);
        if (sessionProbe.confidence > 0) result.probes.add(sessionProbe);

        if (result.probes.isEmpty()) {
            result.status = Status.UNKNOWN_FORMAT;
            return result;
        }

        result.probes.sort(Comparator.comparingDouble((IngressProbe p) -> p.confidence).reversed());
        result.primaryKind = result.probes.get(0).kind;

        if (result.probes.size() >= 2 && config.acceptMixed) {
            float delta = result.probes.get(0).confidence - result.probes.get(1).confidence;
            if (delta < 0.12f) {
                result.isMixedStream = true;
                result.primaryKind = IngressKind.MIXED_STREAM;
            }
        }
        return result;
    }

    public boolean looksLikeFix44(String bytes) {
        return bytes != null && bytes.length() >= 8 && bytes.startsWith("8=FIX.4.4");
    }

    public boolean looksLikeSwiftMt940(String bytes) {
        if (bytes == null) return false;
        return bytes.contains("{1:") || bytes.contains("{4:") || bytes.contains(":20:") || bytes.contains(":61:");
    }

    public boolean looksLikeBatchMagic(int magic) {
        return magic == WireTypes.BATCH_MAGIC;
    }

    public boolean looksLikeEnvelopeMagic(int magic) {
        return magic == WireTypes.ENVELOPE_MAGIC;
    }

    public boolean looksLikeSessionMagic(int magic) {
        return magic == WireTypes.SESSION_MAGIC;
    }

    public int readMagicLe(byte[] data, int offset, int length) {
        if (data == null || length - offset < 4) return 0;
        return BoundsUtil.readU32Le(data, offset);
    }

    public int countPrintablePrefix(byte[] data, int offset, int length) {
        int count = 0;
        for (int i = offset; i < offset + length; i++) {
            char ch = (char) (data[i] & 0xFF);
            if (ch >= 0x20 && ch <= 0x7E || ch == '\r' || ch == '\n' || ch == '\u0001') count++;
            else break;
        }
        return count;
    }

    public IngressProbe probeFix44(String bytes) {
        IngressProbe probe = new IngressProbe();
        probe.kind = IngressKind.FIX44;
        if (bytes.length() < config.minFixHeaderLen || !looksLikeFix44(bytes)) return probe;
        probe.confidence = 0.55f;
        if (bytes.contains("\u00019=")) {
            probe.confidence += 0.20f;
            probe.frameHintBytes = bytes.length();
        }
        if (bytes.contains("\u000135=")) probe.confidence += 0.15f;
        if (bytes.contains("\u000110=")) probe.confidence += 0.10f;
        probe.confidence = clampConfidence(probe.confidence);
        return probe;
    }

    public IngressProbe probeSwift(String bytes) {
        IngressProbe probe = new IngressProbe();
        probe.kind = IngressKind.SWIFT_MT940;
        probe.frameHintBytes = bytes.length();
        if (looksLikeSwiftMt940(bytes)) probe.confidence = 0.50f;
        if (bytes.contains(":61:")) {
            probe.confidence += 0.25f;
            probe.magicOrTag = 61;
        }
        if (bytes.contains(":86:")) probe.confidence += 0.15f;
        if (bytes.contains(":60F:") || bytes.contains(":62F:")) probe.confidence += 0.10f;
        probe.confidence = clampConfidence(probe.confidence);
        return probe;
    }

    public IngressProbe probeBatchWire(byte[] data, int offset, int length) {
        IngressProbe probe = new IngressProbe();
        probe.kind = IngressKind.BATCH_WIRE;
        if (data == null || length - offset < 28) return probe;
        int magic = readMagicLe(data, offset, length);
        probe.magicOrTag = magic;
        if (!looksLikeBatchMagic(magic)) return probe;
        probe.confidence = 0.70f;
        int version = BoundsUtil.readU16Le(data, offset + 4);
        if (version == WireTypes.WIRE_VERSION) probe.confidence += 0.15f;
        int recordCount = readMagicLe(data, offset + 8, length);
        if (recordCount <= WireTypes.MAX_BATCH_RECORDS) {
            probe.confidence += 0.10f;
            probe.frameHintBytes = 28 + recordCount * 32;
        }
        probe.confidence = clampConfidence(probe.confidence);
        return probe;
    }

    public IngressProbe probeEnvelopeWire(byte[] data, int offset, int length) {
        IngressProbe probe = new IngressProbe();
        probe.kind = IngressKind.ENVELOPE_WIRE;
        if (data == null || length - offset < 24) return probe;
        int magic = readMagicLe(data, offset, length);
        probe.magicOrTag = magic;
        if (!looksLikeEnvelopeMagic(magic)) return probe;
        probe.confidence = 0.75f;
        int channelCount = BoundsUtil.readU16Le(data, offset + 6);
        if (channelCount <= WireTypes.MAX_ENVELOPE_CHANNELS) {
            probe.confidence += 0.15f;
            probe.frameHintBytes = 24 + channelCount * 20;
        }
        probe.confidence = clampConfidence(probe.confidence);
        return probe;
    }

    public IngressProbe probeSessionWire(byte[] data, int offset, int length) {
        IngressProbe probe = new IngressProbe();
        probe.kind = IngressKind.SESSION_WIRE;
        if (data == null || length - offset < 24) return probe;
        int magic = readMagicLe(data, offset, length);
        probe.magicOrTag = magic;
        if (!looksLikeSessionMagic(magic)) return probe;
        probe.confidence = 0.72f;
        int legCount = BoundsUtil.readU16Le(data, offset + 6);
        if (legCount <= WireTypes.MAX_SESSION_LEGS) {
            probe.confidence += 0.18f;
            probe.frameHintBytes = 24 + legCount * 28;
        }
        probe.confidence = clampConfidence(probe.confidence);
        return probe;
    }

    public IngressClassifyResult classifyWithComplianceHint(String bytes, boolean requireMarginPath) {
        IngressClassifyResult result = classify(bytes);
        if (result.status != Status.OK || result.probes.isEmpty()) return result;
        for (IngressProbe probe : result.probes) {
            probe.confidence = clampConfidence(probe.confidence + scoreComplianceRelevance(probe, bytes));
        }
        result.probes.sort(Comparator.comparingDouble((IngressProbe p) -> p.confidence).reversed());
        result.primaryKind = result.probes.get(0).kind;
        if (requireMarginPath) {
            boolean marginOk = false;
            for (IngressProbe probe : result.probes) {
                if (probe.kind == IngressKind.SWIFT_MT940 || probe.kind == IngressKind.BATCH_WIRE
                        || probe.kind == IngressKind.ENVELOPE_WIRE) {
                    marginOk = true;
                    break;
                }
            }
            if (!marginOk) result.status = Status.MARGIN_BREACH;
        }
        return result;
    }

    private float scoreComplianceRelevance(IngressProbe probe, String bytes) {
        float bonus = 0f;
        if (probe.kind == IngressKind.FIX44 && probeHasAllocationTags(bytes)) bonus += 0.08f;
        if (probe.kind == IngressKind.SWIFT_MT940 && probeHasMarginSettlementTags(bytes)) bonus += 0.06f;
        if (probe.kind == IngressKind.BATCH_WIRE && probe.magicOrTag == WireTypes.BATCH_MAGIC) bonus += 0.05f;
        return bonus;
    }

    private static boolean probeHasAllocationTags(String bytes) {
        return bytes.contains("\u000178=") || bytes.contains("\u000179=") || bytes.contains("\u0001467=");
    }

    private static boolean probeHasMarginSettlementTags(String bytes) {
        return bytes.contains(":62F:") || bytes.contains(":60F:");
    }

    public float clampConfidence(float value) {
        if (value < 0f) return 0f;
        if (value > 1f) return 1f;
        return value;
    }

    public IngressProbe probe(byte[] data, int offset, int length) {
        IngressClassifyResult result = classifyBinary(data, offset, length);
        return result.probes.isEmpty() ? new IngressProbe() : result.probes.get(0);
    }

    public boolean isBinaryWire(IngressKind kind) {
        return kind == IngressKind.BATCH_WIRE || kind == IngressKind.ENVELOPE_WIRE || kind == IngressKind.SESSION_WIRE;
    }

    public String summarize(IngressClassifyResult result) {
        return "kind=" + result.primaryKind + " mixed=" + result.isMixedStream + " probes=" + result.probes.size();
    }
}
