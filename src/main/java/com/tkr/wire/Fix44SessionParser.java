package com.tkr.wire;

import com.tkr.types.WireTypes.Status;
import com.tkr.util.DigestUtil;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** FIX 4.4 SOH-delimited parser with session state and tag-10 checksum validation. */
public final class Fix44SessionParser {

    public static final char SOH = '\u0001';

    public enum FixSessionPhase {
        DISCONNECTED, AWAIT_LOGON, ACTIVE, AWAIT_LOGOUT, REJECTED, RESEND_GAP, ALLOCATION_PENDING
    }

    public enum FixMsgType {
        UNKNOWN, LOGON, LOGOUT, HEARTBEAT, TEST_REQUEST, RESEND_REQUEST, REJECT,
        SEQUENCE_RESET, NEW_ORDER_SINGLE, EXECUTION_REPORT, ALLOCATION_INSTRUCTION, ALLOCATION_REPORT
    }

    public static final class FixField { public int tag; public String value; }

    public static final class FixAllocationLeg {
        public String allocAccount;
        public String individualAllocId;
        public long allocQtyMilli;
        public long avgPxTicks;
    }

    public static final class FixMessage {
        public FixMsgType msgType = FixMsgType.UNKNOWN;
        public String beginString = "FIX.4.4";
        public int bodyLength;
        public String senderCompId = "";
        public String targetCompId = "";
        public int msgSeqNum;
        public String clOrdId = "";
        public String symbol = "";
        public long lastQtyMilli;
        public long lastPxTicks;
        public List<FixAllocationLeg> allocLegs = new ArrayList<>();
        public List<FixField> rawFields = new ArrayList<>();
        public int checksumTag10;
        public boolean checksumValid;
    }

    public static final class FixSessionState {
        public FixSessionPhase phase = FixSessionPhase.DISCONNECTED;
        public int inboundSeq;
        public int outboundSeq;
        public int nextInboundSeq = 1;
        public boolean resetOnLogon;
        public long lastHeartbeatMillis;
        public int senderCompIdHash;
        public int targetCompIdHash;
        public String lastClOrdId = "";
        public String lastExecId = "";
        public List<FixAllocationLeg> pendingLegs = new ArrayList<>();
    }

    public static final class FixParseResult {
        public Status status = Status.OK;
        public FixMessage message = new FixMessage();
        public int bytesConsumed;
        public boolean sessionUpdated;
    }

    public static final class FixParserConfig {
        public boolean strictChecksum = true;
        public boolean trackSession = true;
        public boolean allowGapFill = true;
        public int maxFields = 512;
    }

    private static final int TAG_BEGIN_STRING = 8, TAG_BODY_LENGTH = 9, TAG_MSG_TYPE = 35;
    private static final int TAG_SENDER_COMP_ID = 49, TAG_TARGET_COMP_ID = 56, TAG_MSG_SEQ_NUM = 34;
    private static final int TAG_CHECKSUM = 10, TAG_CL_ORD_ID = 11, TAG_SYMBOL = 55;
    private static final int TAG_LAST_QTY = 32, TAG_LAST_PX = 31, TAG_ALLOC_ACCOUNT = 79;
    private static final int TAG_INDIVIDUAL_ALLOC_ID = 467, TAG_ALLOC_QTY = 80, TAG_AVG_PX = 6;
    private static final int TAG_GAP_FILL_FLAG = 123, TAG_NO_ALLOCS = 78, TAG_EXEC_ID = 17;
    private static final int TAG_EXEC_TYPE = 150, TAG_ORD_STATUS = 39, TAG_SIDE = 54;
    private static final int TAG_RESET_SEQ_NUM_FLAG = 141, TAG_ORDER_QTY = 38, TAG_PRICE = 44;
    private static final int TAG_NEW_SEQ_NO = 36;

    private final FixParserConfig config;
    private FixSessionState session = new FixSessionState();
    private final Map<Integer, String> tagIndexScratch = new HashMap<>();

    public Fix44SessionParser() { this(new FixParserConfig()); }
    public Fix44SessionParser(FixParserConfig config) {
        this.config = config != null ? config : new FixParserConfig();
        resetSession();
    }

    public FixSessionState getSession() { return session; }
    public void resetSession() { session = new FixSessionState(); session.phase = FixSessionPhase.DISCONNECTED; }
    public void setResetOnLogon(boolean reset) { session.resetOnLogon = reset; }

    public FixParseResult parseMessage(byte[] data, int offset, int length) {
        FixParseResult result = new FixParseResult();
        if (data == null || length <= 0) { result.status = Status.TRUNCATED; return result; }
        List<FixField> fields = tokenizeFields(data, offset, length);
        if (fields.isEmpty()) { result.status = Status.UNKNOWN_FORMAT; return result; }
        FixMessage msg = new FixMessage();
        msg.rawFields = fields;
        Status mapped = mapFields(fields, msg);
        if (mapped != Status.OK) { result.status = mapped; return result; }
        int computed = computeChecksum(data, offset, length);
        msg.checksumTag10 = parseChecksum(fields);
        msg.checksumValid = (computed % 256) == msg.checksumTag10;
        if (config.strictChecksum && !msg.checksumValid) { result.status = Status.BOUNDS_ERROR; return result; }
        if (config.trackSession) result.sessionUpdated = advanceSession(result.message);
        result.message = msg;
        result.bytesConsumed = length;
        return result;
    }

    public FixParseResult parseWithSession(byte[] data, int offset, int length) {
        FixParseResult result = parseMessage(data, offset, length);
        if (result.status != Status.OK) return result;
        Status routeSt = routeMessage(result.message);
        if (routeSt != Status.OK) result.status = routeSt;
        return result;
    }

    public FixParseResult parseWithSession(String raw) {
        byte[] data = raw.getBytes(StandardCharsets.US_ASCII);
        return parseWithSession(data, 0, data.length);
    }

    public FixParseResult parseMessage(byte[] data) {
        return parseMessage(data, 0, data != null ? data.length : 0);
    }

    public FixMsgType msgTypeFromTag(String value) {
        if (value == null) return FixMsgType.UNKNOWN;
        return switch (value) {
            case "A" -> FixMsgType.LOGON; case "5" -> FixMsgType.LOGOUT; case "0" -> FixMsgType.HEARTBEAT;
            case "D" -> FixMsgType.NEW_ORDER_SINGLE; case "8" -> FixMsgType.EXECUTION_REPORT;
            case "J" -> FixMsgType.ALLOCATION_INSTRUCTION; case "AS" -> FixMsgType.ALLOCATION_REPORT;
            case "4" -> FixMsgType.SEQUENCE_RESET; default -> FixMsgType.UNKNOWN;
        };
    }

    private List<FixField> tokenizeFields(byte[] data, int offset, int length) {
        List<FixField> out = new ArrayList<>();
        int end = offset + length, i = offset;
        while (i < end) {
            int eq = -1;
            for (int j = i; j < end; j++) {
                if (data[j] == '=') { eq = j; break; }
                if (data[j] == SOH) break;
            }
            if (eq < 0) break;
            int tag = parseIntAscii(data, i, eq);
            int valStart = eq + 1, valEnd = valStart;
            while (valEnd < end && data[valEnd] != SOH) valEnd++;
            FixField f = new FixField();
            f.tag = tag;
            f.value = new String(data, valStart, valEnd - valStart, StandardCharsets.US_ASCII);
            out.add(f);
            i = valEnd + 1;
        }
        return out;
    }

    private Status mapFields(List<FixField> fields, FixMessage msg) {
        Map<Integer, String> byTag = new HashMap<>();
        for (FixField f : fields) byTag.put(f.tag, f.value);
        msg.msgType = msgTypeFromTag(byTag.get(TAG_MSG_TYPE));
        msg.beginString = byTag.getOrDefault(TAG_BEGIN_STRING, "FIX.4.4");
        msg.senderCompId = byTag.getOrDefault(TAG_SENDER_COMP_ID, "");
        msg.targetCompId = byTag.getOrDefault(TAG_TARGET_COMP_ID, "");
        msg.msgSeqNum = parseIntOrZero(byTag.get(TAG_MSG_SEQ_NUM));
        msg.clOrdId = byTag.getOrDefault(TAG_CL_ORD_ID, "");
        msg.symbol = byTag.getOrDefault(TAG_SYMBOL, "");
        msg.lastQtyMilli = parseQtyToMilli(byTag.get(TAG_LAST_QTY));
        msg.lastPxTicks = parseFixDecimalToTicks(byTag.get(TAG_LAST_PX));
        parseAllocationRepeating(fields, msg);
        return msg.beginString.startsWith("FIX.4.") ? Status.OK : Status.UNKNOWN_FORMAT;
    }

    private void parseAllocationRepeating(List<FixField> fields, FixMessage msg) {
        FixAllocationLeg current = null;
        for (FixField f : fields) {
            if (f.tag == TAG_ALLOC_ACCOUNT) {
                if (current != null) msg.allocLegs.add(current);
                current = new FixAllocationLeg();
                current.allocAccount = f.value;
            } else if (current != null) {
                if (f.tag == TAG_INDIVIDUAL_ALLOC_ID) current.individualAllocId = f.value;
                else if (f.tag == TAG_ALLOC_QTY) current.allocQtyMilli = parseQtyToMilli(f.value);
                else if (f.tag == TAG_AVG_PX) current.avgPxTicks = parseFixDecimalToTicks(f.value);
            }
        }
        if (current != null) msg.allocLegs.add(current);
    }

    private boolean advanceSession(FixMessage msg) {
        buildTagIndex(msg);
        int seq = parseIntOrZero(lookupTag(msg, TAG_MSG_SEQ_NUM));
        if (seq > 0) {
            int expected = session.inboundSeq + 1;
            if (session.inboundSeq != 0 && seq > expected) session.phase = FixSessionPhase.RESEND_GAP;
            session.inboundSeq = seq;
            session.nextInboundSeq = seq + 1;
        }
        String msgType = lookupTag(msg, TAG_MSG_TYPE);
        if (msgType == null) return false;
        return switch (msgType) {
            case "A" -> applyLogon(msg) == Status.OK;
            case "4" -> applySequenceReset(msg) == Status.OK;
            case "8" -> applyExecutionReport(msg) == Status.OK;
            case "J" -> applyAllocationInstruction(msg) == Status.OK;
            case "AS" -> {
                session.phase = FixSessionPhase.ACTIVE;
                session.pendingLegs.clear();
                yield true;
            }
            default -> applySessionRules(msg);
        };
    }

    private void buildTagIndex(FixMessage msg) {
        tagIndexScratch.clear();
        for (FixField f : msg.rawFields) tagIndexScratch.put(f.tag, f.value);
    }

    private String lookupTag(FixMessage msg, int tag) {
        for (FixField f : msg.rawFields) if (f.tag == tag) return f.value;
        return null;
    }

    private boolean lookupTagInt(FixMessage msg, int tag, int[] out) {
        String text = lookupTag(msg, tag);
        if (text == null) return false;
        out[0] = parseIntOrZero(text);
        return true;
    }

    public Status applyLogon(FixMessage msg) {
        String sender = lookupTag(msg, TAG_SENDER_COMP_ID);
        String target = lookupTag(msg, TAG_TARGET_COMP_ID);
        if (sender == null || target == null) return Status.TRUNCATED;
        session.senderCompIdHash = hashCompId(sender);
        session.targetCompIdHash = hashCompId(target);
        int[] resetFlag = new int[1];
        if (lookupTagInt(msg, TAG_RESET_SEQ_NUM_FLAG, resetFlag) && resetFlag[0] != 0) {
            session.inboundSeq = 0;
            session.outboundSeq = 0;
        }
        session.phase = FixSessionPhase.ACTIVE;
        return Status.OK;
    }

    public Status applySequenceReset(FixMessage msg) {
        int[] newSeq = new int[1];
        if (!lookupTagInt(msg, TAG_NEW_SEQ_NO, newSeq) || newSeq[0] < 0) return Status.BOUNDS_ERROR;
        session.inboundSeq = newSeq[0];
        session.phase = FixSessionPhase.ACTIVE;
        return Status.OK;
    }

    public Status applyExecutionReport(FixMessage msg) {
        String clOrd = lookupTag(msg, TAG_CL_ORD_ID);
        if (clOrd != null) session.lastClOrdId = clOrd;
        String execId = lookupTag(msg, TAG_EXEC_ID);
        if (execId != null) session.lastExecId = execId;
        int[] execType = new int[1];
        if (lookupTagInt(msg, TAG_EXEC_TYPE, execType) && (execType[0] == 'F' || execType[0] == '2')) {
            session.phase = FixSessionPhase.ALLOCATION_PENDING;
        }
        return Status.OK;
    }

    public Status applyAllocationInstruction(FixMessage msg) {
        session.pendingLegs.clear();
        List<FixAllocationLeg> legs = extractAllocationLegs(msg);
        session.pendingLegs.addAll(legs);
        session.phase = FixSessionPhase.ALLOCATION_PENDING;
        return Status.OK;
    }

    public Status routeMessage(FixMessage msg) {
        String msgType = lookupTag(msg, TAG_MSG_TYPE);
        if (msgType == null) return Status.TRUNCATED;
        return switch (msgType) {
            case "0" -> processHeartbeat(msg);
            case "5" -> processLogout(msg);
            case "3" -> processReject(msg);
            case "2" -> processResendRequest(msg);
            case "F" -> processOrderCancelRequest(msg);
            case "9" -> processOrderCancelReject(msg);
            case "AS", "AK" -> processAllocationReport(msg);
            case "D" -> processNewOrderSingle(msg);
            case "H" -> processOrderStatusRequest(msg);
            case "AE" -> processTradeCaptureReport(msg);
            case "W" -> processMarketDataSnapshot(msg);
            case "j" -> processBusinessMessageReject(msg);
            default -> Status.OK;
        };
    }

    public Status processHeartbeat(FixMessage msg) {
        int[] testReq = new int[1];
        if (lookupTagInt(msg, 112, testReq)) session.phase = FixSessionPhase.ACTIVE;
        session.lastHeartbeatMillis = System.currentTimeMillis();
        return Status.OK;
    }

    public Status processLogout(FixMessage msg) {
        session.phase = FixSessionPhase.DISCONNECTED;
        session.pendingLegs.clear();
        return Status.OK;
    }

    public Status processReject(FixMessage msg) {
        int[] refSeq = new int[1];
        if (lookupTagInt(msg, 45, refSeq) && refSeq[0] > 0 && refSeq[0] < session.inboundSeq) {
            session.phase = FixSessionPhase.RESEND_GAP;
        }
        return Status.OK;
    }

    public Status processResendRequest(FixMessage msg) {
        int[] beginSeq = new int[1];
        int[] endSeq = new int[1];
        lookupTagInt(msg, 7, beginSeq);
        lookupTagInt(msg, 16, endSeq);
        if (beginSeq[0] > 0) session.phase = FixSessionPhase.RESEND_GAP;
        if (endSeq[0] == 0) session.outboundSeq = session.inboundSeq;
        return Status.OK;
    }

    public Status processGapFill(FixMessage msg) {
        int[] gapFill = new int[1];
        if (lookupTagInt(msg, TAG_GAP_FILL_FLAG, gapFill) && gapFill[0] != 0) {
            return applySequenceReset(msg);
        }
        return Status.OK;
    }

    public Status processAllocationReport(FixMessage msg) {
        List<FixAllocationLeg> legs = extractAllocationLegs(msg);
        session.pendingLegs.addAll(legs);
        session.phase = FixSessionPhase.ACTIVE;
        return Status.OK;
    }

    public Status processOrderCancelRequest(FixMessage msg) {
        Status valid = validateOrderFields(msg);
        if (valid != Status.OK) return valid;
        String clOrd = lookupTag(msg, TAG_CL_ORD_ID);
        if (clOrd != null) session.lastClOrdId = clOrd;
        return Status.OK;
    }

    public Status processOrderCancelReject(FixMessage msg) {
        int[] reason = new int[1];
        if (lookupTagInt(msg, 102, reason) && reason[0] > 0) session.phase = FixSessionPhase.ACTIVE;
        return Status.OK;
    }

    public Status processNewOrderSingle(FixMessage msg) {
        Status valid = validateOrderFields(msg);
        if (valid != Status.OK) return valid;
        String clOrd = lookupTag(msg, TAG_CL_ORD_ID);
        if (clOrd != null) session.lastClOrdId = clOrd;
        int[] orderQty = new int[1];
        int[] side = new int[1];
        lookupTagInt(msg, TAG_ORDER_QTY, orderQty);
        lookupTagInt(msg, TAG_SIDE, side);
        if (orderQty[0] <= 0) return Status.BOUNDS_ERROR;
        if (side[0] != 1 && side[0] != 2) return Status.BOUNDS_ERROR;
        session.phase = FixSessionPhase.ACTIVE;
        return Status.OK;
    }

    public Status processOrderStatusRequest(FixMessage msg) {
        String clOrd = lookupTag(msg, TAG_CL_ORD_ID);
        if (clOrd == null || clOrd.isEmpty()) return Status.TRUNCATED;
        String orderId = lookupTag(msg, 37);
        if ((orderId == null || orderId.isEmpty()) && !clOrd.equals(session.lastClOrdId)) return Status.BOUNDS_ERROR;
        return Status.OK;
    }

    public Status processTradeCaptureReport(FixMessage msg) {
        Status valid = validateExecutionFields(msg);
        if (valid != Status.OK) return valid;
        String execId = lookupTag(msg, TAG_EXEC_ID);
        if (execId != null) session.lastExecId = execId;
        int[] lastQty = new int[1];
        int[] lastPx = new int[1];
        lookupTagInt(msg, TAG_LAST_QTY, lastQty);
        lookupTagInt(msg, TAG_LAST_PX, lastPx);
        FixAllocationLeg leg = new FixAllocationLeg();
        String allocAcct = lookupTag(msg, TAG_ALLOC_ACCOUNT);
        if (allocAcct != null) leg.allocAccount = allocAcct;
        leg.allocQtyMilli = lastQty[0];
        leg.avgPxTicks = lastPx[0];
        if (lastQty[0] > 0) session.pendingLegs.add(leg);
        return Status.OK;
    }

    public Status processMarketDataSnapshot(FixMessage msg) {
        String symbol = lookupTag(msg, TAG_SYMBOL);
        if (symbol == null || symbol.isEmpty()) return Status.TRUNCATED;
        int[] noEntries = new int[1];
        lookupTagInt(msg, 268, noEntries);
        if (noEntries[0] < 0 || noEntries[0] > 1000) return Status.BOUNDS_ERROR;
        return Status.OK;
    }

    public Status processBusinessMessageReject(FixMessage msg) {
        int[] reason = new int[1];
        lookupTagInt(msg, 380, reason);
        if (reason[0] > 0) session.phase = FixSessionPhase.ACTIVE;
        int[] refMsgType = new int[1];
        if (lookupTagInt(msg, 372, refMsgType) && refMsgType[0] == 68) {
            session.phase = FixSessionPhase.ALLOCATION_PENDING;
        }
        return Status.OK;
    }

    public Status validateOrderFields(FixMessage msg) {
        String clOrd = lookupTag(msg, TAG_CL_ORD_ID);
        if (clOrd == null || clOrd.isEmpty()) return Status.TRUNCATED;
        String symbol = lookupTag(msg, TAG_SYMBOL);
        if (symbol == null || symbol.isEmpty()) return Status.TRUNCATED;
        int[] side = new int[1];
        if (!lookupTagInt(msg, TAG_SIDE, side)) return Status.BOUNDS_ERROR;
        if (side[0] != 1 && side[0] != 2) return Status.BOUNDS_ERROR;
        return Status.OK;
    }

    public Status validateExecutionFields(FixMessage msg) {
        String execId = lookupTag(msg, TAG_EXEC_ID);
        if (execId == null || execId.isEmpty()) return Status.TRUNCATED;
        int[] execType = new int[1];
        int[] ordStatus = new int[1];
        if (!lookupTagInt(msg, TAG_EXEC_TYPE, execType)) return Status.BOUNDS_ERROR;
        if (!lookupTagInt(msg, TAG_ORD_STATUS, ordStatus)) return Status.BOUNDS_ERROR;
        if (execType[0] < 0 || execType[0] > 8) return Status.BOUNDS_ERROR;
        if (ordStatus[0] < 0 || ordStatus[0] > 8) return Status.BOUNDS_ERROR;
        return Status.OK;
    }

    public List<FixAllocationLeg> extractAllocationLegs(FixMessage msg) {
        List<FixAllocationLeg> legs = new ArrayList<>();
        int[] groupCount = new int[1];
        if (!lookupTagInt(msg, TAG_NO_ALLOCS, groupCount) || groupCount[0] <= 0) return legs;
        FixAllocationLeg current = null;
        for (FixField field : msg.rawFields) {
            if (field.tag == TAG_NO_ALLOCS) continue;
            if (field.tag == TAG_ALLOC_ACCOUNT) {
                if (current != null) legs.add(current);
                current = new FixAllocationLeg();
                current.allocAccount = field.value;
            } else if (current != null) {
                if (field.tag == TAG_ALLOC_QTY) current.allocQtyMilli = parseQtyToMilli(field.value);
                else if (field.tag == TAG_AVG_PX) current.avgPxTicks = parseFixDecimalToTicks(field.value);
                else if (field.tag == TAG_INDIVIDUAL_ALLOC_ID) current.individualAllocId = field.value;
            }
        }
        if (current != null) legs.add(current);
        return legs;
    }

    private boolean applySessionRules(FixMessage msg) {
        if (session.phase == FixSessionPhase.ACTIVE) {
            if (msg.msgSeqNum != session.nextInboundSeq && !config.allowGapFill) {
                session.phase = FixSessionPhase.REJECTED;
                return false;
            }
            session.nextInboundSeq = Math.max(session.nextInboundSeq, msg.msgSeqNum + 1);
        }
        return true;
    }

    public boolean validateChecksum(String raw) {
        byte[] data = raw.getBytes(StandardCharsets.US_ASCII);
        return validateChecksum(data, 0, data.length);
    }

    public boolean validateChecksum(byte[] data, int offset, int length) {
        int computed = computeChecksum(data, offset, length);
        List<FixField> fields = tokenizeFields(data, offset, length);
        return computed == parseChecksum(fields);
    }

    public static long parseFixDecimalToTicks(String value) {
        if (value == null || value.isEmpty()) return 0;
        boolean neg = value.charAt(0) == '-';
        int start = neg ? 1 : 0;
        long whole = 0, frac = 0; int fracDigits = 0; boolean dot = false;
        for (int i = start; i < value.length(); i++) {
            char ch = value.charAt(i);
            if (ch == '.') { dot = true; continue; }
            if (ch < '0' || ch > '9') break;
            int d = ch - '0';
            if (!dot) whole = whole * 10 + d;
            else if (fracDigits < 6) { frac = frac * 10 + d; fracDigits++; }
        }
        long ticks = whole * 10000L;
        long mul = 10000L;
        for (int i = 0; i < fracDigits; i++) mul /= 10;
        ticks += frac * mul;
        return neg ? -ticks : ticks;
    }

    public static long parseQtyToMilli(String value) { return parseFixDecimalToTicks(value); }

    private int computeChecksum(byte[] data, int offset, int length) {
        int sum = 0, end = offset + length;
        for (int i = offset; i < end; i++) {
            if (data[i] == SOH && i + 4 < end && data[i+1]=='1' && data[i+2]=='0' && data[i+3]=='=') break;
            sum += data[i] & 0xFF;
        }
        return sum % 256;
    }

    private int parseChecksum(List<FixField> fields) {
        for (FixField f : fields) if (f.tag == TAG_CHECKSUM) return parseIntOrZero(f.value) % 256;
        return -1;
    }

    private static int parseIntAscii(byte[] data, int start, int end) {
        int v = 0;
        for (int i = start; i < end; i++) {
            char ch = (char) data[i];
            if (ch < '0' || ch > '9') break;
            v = v * 10 + (ch - '0');
        }
        return v;
    }

    private static int parseIntOrZero(String s) {
        if (s == null || s.isEmpty()) return 0;
        try { return Integer.parseInt(s.trim()); } catch (NumberFormatException e) { return 0; }
    }

    public int hashCompId(String text) {
        return text == null ? DigestUtil.FNV_OFFSET : DigestUtil.fnv1a32(text.getBytes(StandardCharsets.US_ASCII));
    }

    public boolean isSessionActive() { return session.phase == FixSessionPhase.ACTIVE; }
// --- expanded helpers ---
    public boolean validateSequence(int seqNum) {
        return session.phase != FixSessionPhase.ACTIVE || seqNum <= session.nextInboundSeq + 1000;
    }

    public int countAllocationLegs(FixMessage msg) {
        return msg == null ? 0 : msg.allocLegs.size();
    }

    public Status splitFixFrame(byte[] data, int offset, int length, String[] bodyOut, String[] checksumOut) {
        if (data == null || length < 10) return Status.TRUNCATED;
        String raw = new String(data, offset, length, StandardCharsets.US_ASCII);
        if (!raw.startsWith("8=FIX.4.4")) return Status.UNKNOWN_FORMAT;
        int lenTag = raw.indexOf("\u00019=");
        if (lenTag < 0) return Status.TRUNCATED;
        int lenValueStart = lenTag + 3;
        int lenValueEnd = raw.indexOf(SOH, lenValueStart);
        if (lenValueEnd < 0) return Status.TRUNCATED;
        int bodyLength = parseIntOrZero(raw.substring(lenValueStart, lenValueEnd));
        int bodyStart = lenValueEnd + 1;
        if (bodyStart + bodyLength > raw.length()) return Status.BOUNDS_ERROR;
        bodyOut[0] = raw.substring(bodyStart, bodyStart + bodyLength);
        checksumOut[0] = raw.substring(bodyStart + bodyLength);
        return Status.OK;
    }

    public boolean isAllocationMessage(FixMessage msg) {
        String msgType = lookupTag(msg, TAG_MSG_TYPE);
        return "J".equals(msgType) || "AS".equals(msgType) || "AK".equals(msgType);
    }

    public boolean isResendGapActive() {
        return session.phase == FixSessionPhase.RESEND_GAP;
    }

    public List<FixAllocationLeg> pendingAllocationLegs() {
        return new ArrayList<>(session.pendingLegs);
    }

    public void clearPendingLegs() {
        session.pendingLegs.clear();
    }

    public String sessionSummary() {
        return "phase=" + session.phase + " in=" + session.inboundSeq + " out=" + session.outboundSeq
                + " pendingLegs=" + session.pendingLegs.size();
    }
}
