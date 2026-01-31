package com.tkr.fuzz;

import com.tkr.engine.PipelineOrchestrator;

/** Jazzer fuzz target for TKR3 session merge pipeline. */
public final class SessionFuzzer {

    private SessionFuzzer() {}

    public static void fuzzerTestOneInput(byte[] data) {
        if (data == null) return;
        PipelineOrchestrator.mergeSessionLegs(data);
    }
}
