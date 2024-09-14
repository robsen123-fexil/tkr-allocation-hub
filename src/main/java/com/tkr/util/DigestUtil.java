package com.tkr.util;

/**
 * FNV-1a 32-bit digest utilities for payload and merge hashing.
 */
public final class DigestUtil {

    public static final int FNV_OFFSET = (int) 0x811C9DC5L;
    public static final int FNV_PRIME = 16777619;

    private DigestUtil() {}

    public static int fnv1a32(byte[] data, int offset, int length) {
        int hash = FNV_OFFSET;
        if (data == null || length <= 0) {
            return hash;
        }
        int end = offset + length;
        for (int i = offset; i < end; i++) {
            hash ^= (data[i] & 0xFF);
            hash *= FNV_PRIME;
        }
        return hash;
    }

    public static int fnv1a32(byte[] data) {
        if (data == null) {
            return FNV_OFFSET;
        }
        return fnv1a32(data, 0, data.length);
    }

    public static int mixDigest(int hash, int value) {
        hash ^= value;
        hash *= FNV_PRIME;
        return hash;
    }

    public static int combineDigests(int[] digests) {
        int hash = FNV_OFFSET;
        if (digests == null) {
            return hash;
        }
        for (int d : digests) {
            hash = mixDigest(hash, d);
        }
        return hash;
    }

    public static int rollingDigest(int current, byte b) {
        current ^= (b & 0xFF);
        current *= FNV_PRIME;
        return current;
    }

    public static int digestHeaderFields(int magic, int version, int count, int flags) {
        int hash = FNV_OFFSET;
        hash = mixDigest(hash, magic);
        hash = mixDigest(hash, version);
        hash = mixDigest(hash, count);
        hash = mixDigest(hash, flags);
        return hash;
    }
}
