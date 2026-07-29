package com.tkr.fuzz;

import com.tkr.engine.PipelineOrchestrator;
import com.tkr.ledger.ChannelTape;
import com.tkr.nativelink.NativeBridge;

/** Jazzer fuzz target for ingress router pipeline. */
public final class RouterFuzzer {

    private RouterFuzzer() {}

    public static void fuzzerTestOneInput(byte[] data) {
        if (data == null) return;
        NativeBridge.requireNative();
        NativeBridge.nativeResetState();
        ChannelTape.global().clearPending();
        PipelineOrchestrator.runRouterPipeline(data);
    }
}
