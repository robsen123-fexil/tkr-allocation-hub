package com.tkr.fuzz;

import com.tkr.engine.PipelineOrchestrator;
import com.tkr.nativelink.NativeBridge;

/** Jazzer fuzz target for TKR1 batch wire decode + allocation pipeline. */
public final class BatchFuzzer {

    private BatchFuzzer() {}

    public static void fuzzerTestOneInput(byte[] data) {
        if (data == null) return;
        NativeBridge.requireNative();
        NativeBridge.nativeResetState();
        PipelineOrchestrator.runAllocationPipeline(data);
    }
}
