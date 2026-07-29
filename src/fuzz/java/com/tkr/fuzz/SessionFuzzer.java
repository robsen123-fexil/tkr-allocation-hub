package com.tkr.fuzz;

import com.tkr.engine.PipelineOrchestrator;
import com.tkr.nativelink.NativeBridge;

/** Jazzer fuzz target for TKR3 session merge pipeline. */
public final class SessionFuzzer {

    private SessionFuzzer() {}

    public static void fuzzerTestOneInput(byte[] data) {
        if (data == null) return;
        NativeBridge.nativeResetState();
        PipelineOrchestrator.mergeSessionLegs(data);
    }
}
