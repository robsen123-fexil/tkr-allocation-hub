package com.tkr.nativelink;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;

/**
 * JNI bridge to libtkr_native.so for native digest flush paths.
 * Native code contains intentional UAF bug sites exercised by fuzzers.
 * Maps to JNI symbols Java_com_tkr_nativelink_NativeBridge_* in tkr_native.cpp.
 */
public final class NativeBridge {

    private static volatile boolean loaded;
    private static volatile boolean nativeAvailable;

    static {
        loadNativeLibrary();
    }

    private NativeBridge() {}

    public static synchronized void loadNativeLibrary() {
        if (loaded) {
            return;
        }
        String libName = System.mapLibraryName("tkr_native");

        String envLib = System.getenv("TKR_NATIVE_LIB");
        if (envLib != null && !envLib.isEmpty()) {
            tryLoad(envLib);
        }
        String explicitLib = System.getProperty("tkr.native.lib");
        if (!nativeAvailable && explicitLib != null && !explicitLib.isEmpty()) {
            tryLoad(explicitLib);
        }
        String customPath = System.getProperty("tkr.native.path");
        if (!nativeAvailable && customPath != null && !customPath.isEmpty()) {
            tryLoad(customPath + "/" + libName);
        }

        if (!nativeAvailable) {
            String cwd = System.getProperty("user.dir", ".");
            tryLoad(Path.of(cwd, "native", libName).toString());
            tryLoad(Path.of(cwd, libName).toString());
        }

        if (!nativeAvailable) {
            try {
                System.loadLibrary("tkr_native");
                nativeAvailable = true;
            } catch (UnsatisfiedLinkError ignored) {
                // fall through
            }
        }

        if (!nativeAvailable) {
            String libraryPath = System.getProperty("java.library.path", "");
            for (String dir : libraryPath.split(java.io.File.pathSeparator)) {
                if (dir.isEmpty()) {
                    continue;
                }
                tryLoad(Path.of(dir, libName).toString());
                if (nativeAvailable) {
                    break;
                }
            }
        }

        if (!nativeAvailable) {
            loadNativeFromJar("/native/" + libName);
        }

        loaded = true;
    }

    public static void requireNative() {
        loadNativeLibrary();
        if (!nativeAvailable) {
            throw new UnsatisfiedLinkError("libtkr_native.so could not be loaded");
        }
    }

    private static void tryLoad(String absolutePath) {
        try {
            System.load(absolutePath);
            nativeAvailable = true;
        } catch (UnsatisfiedLinkError ignored) {
            // try next candidate path
        }
    }

    private static void loadNativeFromJar(String resourcePath) {
        try (InputStream in = NativeBridge.class.getResourceAsStream(resourcePath)) {
            if (in == null) {
                return;
            }
            Path tmp = Files.createTempFile("tkr_native_", ".so");
            tmp.toFile().deleteOnExit();
            Files.copy(in, tmp, StandardCopyOption.REPLACE_EXISTING);
            System.load(tmp.toAbsolutePath().toString());
            nativeAvailable = true;
        } catch (IOException | UnsatisfiedLinkError ignored) {
            // extraction or load failed
        }
    }

    public static boolean isNativeLoaded() {
        return nativeAvailable;
    }

    /** Stores bytes in native heap and returns the stable pointer for the new slot. */
    public static long storeHeapBufferAndGetPtr(byte[] data) {
        requireNative();
        nativeStoreHeapBuffer(data);
        return nativeHeapBufferPtr(nativeHeapBufferCount() - 1);
    }

    public static int nativeHeapBufferCount() {
        requireNative();
        return nativeHeapBufferCountNative();
    }

    private static native int nativeHeapBufferCountNative();

    public static native void nativeRegisterBatchSlots(long[] ptrs, int[] lens, int[] recordIds);
    public static native void nativeCompactBatchPayload(byte[] payload);
    public static native int nativeFlushBatchDigest();
    public static native void nativeQueueChannelEntry(long ptr, int len);
    public static native void nativeCommitIngressSweep();
    public static native void nativeStoreHeapBuffer(byte[] data);
    public static native long nativeHeapBufferPtr(int index);
    public static native int nativeSealDeferredEnvelope();
    public static native void nativeRegisterMergeSlots(long[] ptrs, int[] lens, int[] legIds);
    public static native void nativeGraftSessionLegs(byte[] refBlob);
    public static native int nativeFlushMergeDigest();

    public static void nativeResetState() {
        if (!nativeAvailable) {
            return;
        }
        nativeResetStateNative();
    }

    private static native void nativeResetStateNative();

    public static void registerBatchSlotsFromJava(com.tkr.types.WireTypes.DeferredSlot[] slots) {
        requireNative();
        if (slots == null || slots.length == 0) {
            nativeRegisterBatchSlots(new long[0], new int[0], new int[0]);
            return;
        }
        long[] ptrs = new long[slots.length];
        int[] lens = new int[slots.length];
        int[] ids = new int[slots.length];
        for (int i = 0; i < slots.length; i++) {
            com.tkr.types.WireTypes.DeferredSlot s = slots[i];
            ids[i] = s.recordId;
            lens[i] = s.payloadLen;
            if (s.payload != null && s.active && s.payloadLen > 0) {
                nativeStoreHeapBuffer(s.payload);
                ptrs[i] = nativeHeapBufferPtr(nativeHeapBufferCount() - 1);
            } else {
                ptrs[i] = 0;
            }
        }
        nativeRegisterBatchSlots(ptrs, lens, ids);
    }

    public static void registerMergeSlotsFromJava(java.util.List<com.tkr.types.WireTypes.MergeSlot> slots) {
        requireNative();
        if (slots == null || slots.isEmpty()) {
            nativeRegisterMergeSlots(new long[0], new int[0], new int[0]);
            return;
        }
        int n = slots.size();
        long[] ptrs = new long[n];
        int[] lens = new int[n];
        int[] legIds = new int[n];
        for (int i = 0; i < n; i++) {
            com.tkr.types.WireTypes.MergeSlot s = slots.get(i);
            legIds[i] = s.legId;
            lens[i] = s.refLen;
            if (s.refData != null && s.pinned && s.refLen > 0) {
                nativeStoreHeapBuffer(s.refData);
                ptrs[i] = nativeHeapBufferPtr(nativeHeapBufferCount() - 1);
            } else {
                ptrs[i] = 0;
            }
        }
        nativeRegisterMergeSlots(ptrs, lens, legIds);
    }
}
