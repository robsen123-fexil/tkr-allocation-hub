package com.tkr.fuzz;

import com.tkr.engine.PipelineOrchestrator;
import com.tkr.wire.BatchWireCodec;

/** Jazzer fuzz target for TKR1 batch wire decode + allocation pipeline. */
public final class BatchFuzzer {

    private BatchFuzzer() {}

    public static void fuzzerTestOneInput(byte[] data) {
        if (data == null) return;
        BatchWireCodec codec = new BatchWireCodec();
        BatchWireCodec.BatchDecodeResult decoded = codec.decodeBatch(data);
        if (decoded.status == com.tkr.types.WireTypes.Status.OK
                && (decoded.frame.header.flags & com.tkr.types.WireTypes.BATCH_FLAG_DEFERRED_DIGEST) != 0) {
            PipelineOrchestrator.runAllocationPipeline(data);
        }
    }
}
