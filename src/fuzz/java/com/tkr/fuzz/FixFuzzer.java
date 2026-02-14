package com.tkr.fuzz;

import com.tkr.wire.Fix44SessionParser;

/** Jazzer fuzz target for FIX 4.4 session parser. */
public final class FixFuzzer {

    private FixFuzzer() {}

    public static void fuzzerTestOneInput(byte[] data) {
        if (data == null) return;
        Fix44SessionParser parser = new Fix44SessionParser();
        parser.parseMessage(data);
    }
}
