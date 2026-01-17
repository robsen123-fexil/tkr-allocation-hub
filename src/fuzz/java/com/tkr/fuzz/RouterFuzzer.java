package com.tkr.fuzz;

import com.tkr.engine.PipelineOrchestrator;

/** Jazzer fuzz target for ingress router pipeline. */
public final class RouterFuzzer {

    private RouterFuzzer() {}

    public static void fuzzerTestOneInput(byte[] data) {
        if (data == null) return;
        PipelineOrchestrator.runRouterPipeline(data);
    }
}
