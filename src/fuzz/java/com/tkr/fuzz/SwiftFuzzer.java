package com.tkr.fuzz;

import com.tkr.wire.SwiftMt940Scanner;

/** Jazzer fuzz target for SWIFT MT940 block-4 scanner. */
public final class SwiftFuzzer {

    private SwiftFuzzer() {}

    public static void fuzzerTestOneInput(byte[] data) {
        if (data == null) return;
        SwiftMt940Scanner scanner = new SwiftMt940Scanner();
        scanner.scanBlock4(data);
    }
}
