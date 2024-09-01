package com.tkr.util;

/**
 * Bounds and section validation for wire frame parsing.
 */
public final class BoundsUtil {

    private BoundsUtil() {}

    public static boolean sliceInBounds(int offset, int length, int containerSize) {
        if (offset < 0 || length < 0) {
            return false;
        }
        long end = (long) offset + (long) length;
        return end <= containerSize && end >= 0;
    }

    public static boolean sectionBodyInBounds(int sectionOffset, long bodyBytes, long totalSize) {
        if (sectionOffset < 0 || bodyBytes < 0) {
            return false;
        }
        long end = (long) sectionOffset + bodyBytes;
        return end <= totalSize && end >= 0;
    }

    public static boolean indexInRange(int index, int size) {
        return index >= 0 && index < size;
    }

    public static int safeAdd(int a, int b) {
        long sum = (long) a + (long) b;
        if (sum > Integer.MAX_VALUE) {
            return Integer.MAX_VALUE;
        }
        if (sum < Integer.MIN_VALUE) {
            return Integer.MIN_VALUE;
        }
        return (int) sum;
    }

    public static int clamp(int value, int min, int max) {
        if (value < min) {
            return min;
        }
        if (value > max) {
            return max;
        }
        return value;
    }

    public static long clampLong(long value, long min, long max) {
        if (value < min) {
            return min;
        }
        if (value > max) {
            return max;
        }
        return value;
    }

    public static boolean nonOverlapping(int offA, int lenA, int offB, int lenB) {
        long endA = (long) offA + lenA;
        long endB = (long) offB + lenB;
        return endA <= offB || endB <= offA;
    }

    public static int alignUp(int value, int alignment) {
        if (alignment <= 0) {
            return value;
        }
        int remainder = value % alignment;
        if (remainder == 0) {
            return value;
        }
        return value + (alignment - remainder);
    }

    public static boolean fitsUint32(long value) {
        return value >= 0 && value <= 0xFFFFFFFFL;
    }

    public static int readU32Le(byte[] data, int offset) {
        return (data[offset] & 0xFF)
                | ((data[offset + 1] & 0xFF) << 8)
                | ((data[offset + 2] & 0xFF) << 16)
                | ((data[offset + 3] & 0xFF) << 24);
    }

    public static int readU16Le(byte[] data, int offset) {
        return (data[offset] & 0xFF) | ((data[offset + 1] & 0xFF) << 8);
    }

    public static void writeU32Le(byte[] dst, int offset, int value) {
        dst[offset] = (byte) (value & 0xFF);
        dst[offset + 1] = (byte) ((value >> 8) & 0xFF);
        dst[offset + 2] = (byte) ((value >> 16) & 0xFF);
        dst[offset + 3] = (byte) ((value >> 24) & 0xFF);
    }

    public static void writeU16Le(byte[] dst, int offset, int value) {
        dst[offset] = (byte) (value & 0xFF);
        dst[offset + 1] = (byte) ((value >> 8) & 0xFF);
    }

    /** Validates a fixed-width record table fits inside a wire frame body. */
    public static boolean recordTableInBounds(int tableOffset, int recordCount, int recordWidth,
                                             int frameSize) {
        if (recordCount < 0 || recordWidth <= 0 || tableOffset < 0) {
            return false;
        }
        long tableBytes = (long) recordCount * (long) recordWidth;
        return sectionBodyInBounds(tableOffset, tableBytes, frameSize);
    }
}
