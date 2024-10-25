package com.tkr.wire;

import com.tkr.types.WireTypes.Status;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/** SWIFT MT940 block-4 tag scanner for statement lines and balances. */
public final class SwiftMt940Scanner {

    public enum Mt940Tag {
        TAG_20, TAG_25, TAG_28C, TAG_60F, TAG_60M, TAG_61, TAG_86, TAG_62F, TAG_62M, UNKNOWN
    }

    public static final class Mt940Balance {
        public char dcMark;
        public String date = "";
        public String currency = "";
        public long amountMicro;
        public boolean opening;
    }

    public static final class Mt940StatementLine {
        public String valueDate = "";
        public String entryDate = "";
        public char dcMark;
        public long amountMicro;
        public String transactionType = "";
        public String reference = "";
        public String supplementary = "";
    }

    public static final class Mt940Statement {
        public String transactionRef = "";
        public String accountId = "";
        public String statementNumber = "";
        public Mt940Balance openingBalance;
        public Mt940Balance closingBalance;
        public List<Mt940StatementLine> lines = new ArrayList<>();
        public List<String> infoLines = new ArrayList<>();
    }

    public static final class Mt940ScanResult {
        public Status status = Status.OK;
        public Mt940Statement statement = new Mt940Statement();
        public int tagsParsed;
        public int consumedBytes;
        public int lineCount;
    }

    public static final class Mt940ScannerConfig {
        public boolean strictBlock4 = true;
        public int maxLines = 4096;
        public boolean parseCommaDecimal = true;
    }

    private final Mt940ScannerConfig config;

    public SwiftMt940Scanner() { this(new Mt940ScannerConfig()); }
    public SwiftMt940Scanner(Mt940ScannerConfig config) {
        this.config = config != null ? config : new Mt940ScannerConfig();
    }

    public Mt940ScanResult scanBlock4(byte[] data, int offset, int length) {
        Mt940ScanResult result = new Mt940ScanResult();
        if (data == null || length <= 0) { result.status = Status.TRUNCATED; return result; }
        String text = new String(data, offset, length, StandardCharsets.US_ASCII);
        int blockStart = text.indexOf("{4:");
        if (blockStart >= 0) {
            int blockEnd = text.indexOf("-}", blockStart);
            text = blockEnd > blockStart ? text.substring(blockStart + 3, blockEnd) : text.substring(blockStart + 3);
        }
        List<TagSlice> tags = splitTags(text);
        if (tags.size() > config.maxLines) { result.status = Status.BOUNDS_ERROR; return result; }
        Mt940Statement stmt = new Mt940Statement();
        for (TagSlice tag : tags) {
            Mt940Tag kind = classifyTag(tag.name);
            result.tagsParsed++;
            switch (kind) {
                case TAG_20 -> stmt.transactionRef = tag.body.trim();
                case TAG_25 -> stmt.accountId = tag.body.trim();
                case TAG_28C -> stmt.statementNumber = tag.body.trim();
                case TAG_60F, TAG_60M -> {
                    Mt940Balance bal = parseBalance(tag.body);
                    bal.opening = true;
                    stmt.openingBalance = bal;
                }
                case TAG_62F, TAG_62M -> {
                    Mt940Balance bal = parseBalance(tag.body);
                    bal.opening = false;
                    stmt.closingBalance = bal;
                }
                case TAG_61 -> stmt.lines.add(parseStatementLine(tag.body));
                case TAG_86 -> stmt.infoLines.add(tag.body.trim());
                default -> { }
            }
        }
        result.statement = stmt;
        return result;
    }

    public Mt940ScanResult scanBlock4(byte[] data) {
        return scanBlock4(data, 0, data != null ? data.length : 0);
    }

    public Mt940ScanResult scanBlock4(String text) {
        return scanBlock4(text.getBytes(StandardCharsets.US_ASCII));
    }

    private static final class TagSlice { String name; String body; }

    private List<TagSlice> splitTags(String text) {
        List<TagSlice> out = new ArrayList<>();
        int i = 0;
        while (i < text.length()) {
            if (text.charAt(i) != ':') { i++; continue; }
            int nameEnd = text.indexOf(':', i + 1);
            if (nameEnd < 0) break;
            String name = text.substring(i, nameEnd + 1);
            int nextTag = text.indexOf("\r\n:", nameEnd + 1);
            if (nextTag < 0) nextTag = text.indexOf("\n:", nameEnd + 1);
            String body = nextTag < 0 ? text.substring(nameEnd + 1) : text.substring(nameEnd + 1, nextTag);
            TagSlice slice = new TagSlice();
            slice.name = name;
            slice.body = body;
            out.add(slice);
            i = nextTag < 0 ? text.length() : nextTag + (text.charAt(nextTag) == '\r' ? 2 : 1);
        }
        return out;
    }

    private Mt940Tag classifyTag(String name) {
        if (name == null) return Mt940Tag.UNKNOWN;
        return switch (name) {
            case ":20:" -> Mt940Tag.TAG_20;
            case ":25:" -> Mt940Tag.TAG_25;
            case ":28C:" -> Mt940Tag.TAG_28C;
            case ":60F:" -> Mt940Tag.TAG_60F;
            case ":60M:" -> Mt940Tag.TAG_60M;
            case ":61:" -> Mt940Tag.TAG_61;
            case ":86:" -> Mt940Tag.TAG_86;
            case ":62F:" -> Mt940Tag.TAG_62F;
            case ":62M:" -> Mt940Tag.TAG_62M;
            default -> Mt940Tag.UNKNOWN;
        };
    }

    public Mt940Balance parseBalance(String body) {
        Mt940Balance bal = new Mt940Balance();
        if (body == null || body.length() < 10) return bal;
        bal.dcMark = body.charAt(0);
        bal.date = body.substring(1, 7);
        bal.currency = body.substring(7, 10);
        String amountPart = body.substring(10).replace(",", ".");
        bal.amountMicro = parseAmountMicro(amountPart);
        return bal;
    }

    public Mt940StatementLine parseStatementLine(String body) {
        Mt940StatementLine line = new Mt940StatementLine();
        if (body == null || body.length() < 6) return line;
        line.valueDate = body.substring(0, 6);
        int idx = 6;
        if (body.length() > 10 && Character.isDigit(body.charAt(10))) {
            line.entryDate = body.substring(6, 10);
            idx = 10;
        }
        line.dcMark = body.charAt(idx);
        idx++;
        int refStart = body.indexOf('N', idx);
        if (refStart < 0) refStart = body.indexOf('F', idx);
        String amtStr = refStart > idx ? body.substring(idx, refStart) : body.substring(idx);
        line.amountMicro = parseAmountMicro(amtStr.replace(",", "."));
        if (refStart >= 0 && refStart + 3 < body.length()) {
            line.transactionType = body.substring(refStart, refStart + 3);
            line.reference = body.substring(refStart + 3).trim();
        }
        return line;
    }

    public long parseAmountMicro(String amount) {
        if (amount == null || amount.isEmpty()) return 0;
        boolean neg = amount.endsWith("D") || amount.startsWith("-");
        String cleaned = amount.replace("D", "").replace("C", "").replace("-", "").trim();
        long whole = 0, frac = 0;
        int dot = cleaned.indexOf('.');
        if (dot < 0) {
            whole = parseLongSafe(cleaned);
        } else {
            whole = parseLongSafe(cleaned.substring(0, dot));
            String f = cleaned.substring(dot + 1);
            frac = parseLongSafe(f);
            for (int i = f.length(); i < 6; i++) frac *= 10;
        }
        long micro = whole * 1_000_000L + frac;
        return neg ? -micro : micro;
    }

    private long parseLongSafe(String s) {
        try { return Long.parseLong(s.isEmpty() ? "0" : s); } catch (NumberFormatException e) { return 0; }
    }

    public long sumStatementLines(Mt940Statement stmt) {
        long sum = 0;
        for (Mt940StatementLine line : stmt.lines) {
            sum += line.dcMark == 'D' ? -line.amountMicro : line.amountMicro;
        }
        return sum;
    }

    public boolean reconcileBalances(Mt940Statement stmt) {
        if (stmt.openingBalance == null || stmt.closingBalance == null) return false;
        long open = stmt.openingBalance.dcMark == 'D' ? -stmt.openingBalance.amountMicro : stmt.openingBalance.amountMicro;
        long close = stmt.closingBalance.dcMark == 'D' ? -stmt.closingBalance.amountMicro : stmt.closingBalance.amountMicro;
        return open + sumStatementLines(stmt) == close;
    }
// --- expanded helpers ---
    public int countTags(Mt940ScanResult result) { return result.tagsParsed; }

    public String formatStatementSummary(Mt940Statement stmt) {
        return stmt.transactionRef + "|" + stmt.accountId + "|lines=" + stmt.lines.size();
    }

    public static final class Mt940InfoSegment {
        public String rawCode = "";
        public String narrative = "";
        public List<String> subfields = new ArrayList<>();
    }

    public Mt940ScanResult scan(String swiftText) {
        Mt940ScanResult result = new Mt940ScanResult();
        result.status = Status.OK;
        if (swiftText == null || swiftText.isEmpty()) {
            result.status = Status.TRUNCATED;
            return result;
        }
        int block4Start = swiftText.indexOf("{4:");
        String text = swiftText;
        if (block4Start >= 0) {
            int contentStart = block4Start + 3;
            int block4End = swiftText.indexOf("-}", contentStart);
            if (block4End < 0) {
                result.status = Status.TRUNCATED;
                return result;
            }
            text = swiftText.substring(contentStart, block4End);
            result.consumedBytes = block4End + 2;
        } else {
            result.consumedBytes = swiftText.length();
        }
        result.status = scanBlock4(text, result.statement);
        result.lineCount = result.statement.lines.size();
        result.tagsParsed = fieldsSeen;
        if (result.consumedBytes == 0) result.consumedBytes = swiftText.length();
        return result;
    }

    private int fieldsSeen;

    public Status scanBlock4(String block4, Mt940Statement out) {
        fieldsSeen = 0;
        int cursor = 0;
        while (cursor < block4.length()) {
            char ch = block4.charAt(cursor);
            if (ch == '-' || ch == '\r' || ch == '\n') {
                cursor++;
                continue;
            }
            if (ch != ':') {
                cursor++;
                continue;
            }
            int tagEnd = block4.indexOf(':', cursor + 1);
            if (tagEnd < 0) break;
            String tag = block4.substring(cursor, tagEnd + 1);
            cursor = tagEnd + 1;
            int valueStart = cursor;
            int valueEnd = block4.length();
            int nextTag = block4.indexOf("\n:", cursor);
            if (nextTag >= 0) valueEnd = nextTag;
            String value = trimSwift(block4.substring(valueStart, valueEnd));
            Status st = scanTaggedField(tag, value, out);
            if (st != Status.OK) return st;
            cursor = valueEnd;
        }
        return Status.OK;
    }

    public Status scanTaggedField(String tag, String value, Mt940Statement out) {
        return switch (tag) {
            case ":20:" -> {
                out.transactionRef = value;
                fieldsSeen++;
                yield Status.OK;
            }
            case ":25:" -> {
                out.accountId = value;
                fieldsSeen++;
                yield Status.OK;
            }
            case ":28C:", ":28:" -> {
                out.statementNumber = value;
                fieldsSeen++;
                yield Status.OK;
            }
            case ":60F:", ":60M:" -> {
                fieldsSeen++;
                out.openingBalance = parseBalanceField(value);
                out.openingBalance.opening = true;
                yield Status.OK;
            }
            case ":62F:", ":62M:" -> {
                fieldsSeen++;
                out.closingBalance = parseBalanceField(value);
                out.closingBalance.opening = false;
                yield Status.OK;
            }
            case ":61:" -> {
                Mt940StatementLine line = parseStatementLineField(value);
                out.lines.add(line);
                fieldsSeen++;
                yield Status.OK;
            }
            case ":86:" -> {
                out.infoLines.add(parseField86(value));
                fieldsSeen++;
                yield Status.OK;
            }
            default -> Status.OK;
        };
    }

    private Mt940Balance parseBalanceField(String body) {
        Mt940Balance bal = parseBalance(body);
        return bal;
    }

    private String parseField86(String body) {
        Mt940InfoSegment seg = new Mt940InfoSegment();
        scanField86(body, seg);
        return seg.narrative.isEmpty() ? body.trim() : seg.narrative;
    }

    public Status scanField86(String fieldBody, Mt940InfoSegment out) {
        out.rawCode = "";
        out.narrative = "";
        out.subfields.clear();
        String body = trimSwift(fieldBody);
        if (body.isEmpty()) return Status.TRUNCATED;
        if (body.length() >= 3 && body.charAt(0) == '/' && Character.isLetter(body.charAt(1))) {
            int endCode = body.indexOf('/', 2);
            if (endCode >= 0) {
                out.rawCode = body.substring(0, endCode);
                body = body.substring(endCode);
            }
        }
        splitSubfields86(body, out.subfields);
        if (!out.subfields.isEmpty()) out.narrative = out.subfields.get(out.subfields.size() - 1);
        else out.narrative = body;
        return Status.OK;
    }

    public Status splitSubfields86(String body, List<String> subfields) {
        subfields.clear();
        int cursor = 0;
        while (cursor < body.length()) {
            if (cursor + 3 <= body.length() && body.charAt(cursor) == '/'
                    && Character.isLetter(body.charAt(cursor + 1))) {
                int next = body.indexOf('/', cursor + 2);
                if (next < 0) {
                    subfields.add(body.substring(cursor));
                    break;
                }
                subfields.add(body.substring(cursor, next));
                cursor = next;
                continue;
            }
            int nl = body.indexOf('\n', cursor);
            if (nl < 0) {
                subfields.add(body.substring(cursor));
                break;
            }
            subfields.add(body.substring(cursor, nl));
            cursor = nl + 1;
        }
        return Status.OK;
    }

    public Mt940StatementLine parseStatementLineField(String fieldBody) {
        Mt940StatementLine out = new Mt940StatementLine();
        if (fieldBody == null || fieldBody.length() < 12) return out;
        int cursor = 0;
        out.valueDate = fieldBody.substring(cursor, cursor + 6);
        cursor += 6;
        if (cursor + 4 <= fieldBody.length() && Character.isDigit(fieldBody.charAt(cursor))) {
            out.entryDate = fieldBody.substring(cursor, cursor + 4);
            cursor += 4;
        }
        out.dcMark = fieldBody.charAt(cursor++);
        if (cursor < fieldBody.length() && "CDRS".indexOf(fieldBody.charAt(cursor)) >= 0) cursor++;
        int amountStart = cursor;
        while (cursor < fieldBody.length() && (Character.isDigit(fieldBody.charAt(cursor)) || fieldBody.charAt(cursor) == ',')) {
            cursor++;
        }
        out.amountMicro = parseAmountMicro(fieldBody.substring(amountStart, cursor).replace(",", "."));
        if (cursor + 4 <= fieldBody.length()) {
            out.transactionType = fieldBody.substring(cursor, cursor + 4);
            cursor += 4;
        }
        if (cursor < fieldBody.length()) {
            String refs = fieldBody.substring(cursor).trim();
            int slash = refs.indexOf("//");
            if (slash >= 0) {
                out.reference = refs.substring(0, slash);
                out.supplementary = refs.substring(slash + 2);
            } else {
                out.reference = refs;
            }
        }
        return out;
    }

    public long parseAmountMinor(String amountText, char debitCredit) {
        long minor = parseAmountMicro(amountText.replace(",", ".")) / 10000L;
        if (debitCredit == 'D' || debitCredit == 'd') minor = -minor;
        return minor;
    }

    private static String trimSwift(String text) {
        int start = 0;
        int end = text.length();
        while (start < end && (text.charAt(start) == '\r' || text.charAt(start) == '\n' || text.charAt(start) == ' ')) start++;
        while (end > start && (text.charAt(end - 1) == '\r' || text.charAt(end - 1) == '\n' || text.charAt(end - 1) == ' ')) end--;
        return text.substring(start, end);
    }

    public static boolean isDebitCredit(char dc) {
        return dc == 'C' || dc == 'D' || dc == 'R' || dc == 'c' || dc == 'd';
    }
}
