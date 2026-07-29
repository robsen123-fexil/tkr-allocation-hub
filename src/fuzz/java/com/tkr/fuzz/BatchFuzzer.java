package com.tkr.fuzz;

import com.tkr.engine.BatchDigestEngine;
import com.tkr.nativelink.NativeBridge;
import com.tkr.types.WireTypes;
import com.tkr.wire.BatchWireCodec;

/** Jazzer fuzz target for TKR1 batch wire decode + deferred digest flush. */
public final class BatchFuzzer {

    private BatchFuzzer() {}

    public static void fuzzerTestOneInput(byte[] data) {
        if (data == null) return;
        NativeBridge.nativeResetState();
        BatchWireCodec codec = new BatchWireCodec();
        BatchWireCodec.BatchDecodeResult decoded = codec.decodeBatch(data);
        if (decoded.status != WireTypes.Status.OK) return;
        if ((decoded.frame.header.flags & WireTypes.BATCH_FLAG_DEFERRED_DIGEST) == 0) return;
        if (decoded.frame.deferredSlots.isEmpty()) return;
        BatchDigestEngine digest = new BatchDigestEngine();
        digest.registerDeferredSlots(decoded.frame.deferredSlots);
        digest.flushBatchDigest();
    }
}
