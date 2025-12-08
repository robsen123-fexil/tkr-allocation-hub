package com.tkr.tools;

import com.tkr.desk.ProRataAllocator;
import com.tkr.engine.PipelineOrchestrator;
import com.tkr.types.WireTypes;
import com.tkr.types.WireTypes.*;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

/** CLI entry point for tkr-allocation-hub JVM tools. */
public final class TkrCtl {

    private TkrCtl() {}

    public static void main(String[] args) {
        if (args.length == 0) {
            printUsage();
            System.exit(1);
        }
        int code = switch (args[0]) {
            case "allocate" -> cmdAllocate(args);
            case "pipeline" -> cmdPipeline(args);
            case "router" -> cmdRouter(args);
            case "merge" -> cmdMerge(args);
            case "stats" -> cmdStats();
            default -> { printUsage(); yield 1; }
        };
        System.exit(code);
    }

    private static void printUsage() {
        System.err.println("Usage: tkrctl <command> [args]");
        System.err.println("Commands:");
        System.err.println("  allocate <total_qty> <weight1> <weight2> ...  Pro-rata allocation");
        System.err.println("  pipeline <file>                               Run allocation pipeline");
        System.err.println("  router <file>                                 Run router pipeline");
        System.err.println("  merge <file>                                  Merge session legs");
        System.err.println("  stats                                         Show pipeline stats");
    }

    private static int cmdAllocate(String[] args) {
        if (args.length < 3) {
            System.err.println("allocate requires total_qty and weights");
            return 1;
        }
        int totalQty = parseInt(args[1], -1);
        if (totalQty <= 0) return 1;
        List<ProRataAllocator.ProRataWeight> weights = new ArrayList<>();
        List<WireBatchRecord> records = new ArrayList<>();
        for (int i = 2; i < args.length; i++) {
            int w = parseInt(args[i], -1);
            if (w <= 0) return 1;
            ProRataAllocator.ProRataWeight pw = new ProRataAllocator.ProRataWeight();
            pw.accountId = 1000 + i;
            pw.weightBp = w * 100;
            weights.add(pw);
            WireBatchRecord rec = new WireBatchRecord();
            rec.recordId = i;
            rec.accountId = pw.accountId;
            rec.qtyMilli = pw.weightBp;
            records.add(rec);
        }
        ProRataAllocator allocator = new ProRataAllocator();
        ProRataAllocator.ProRataAllocatorResult result =
                allocator.allocateWithWeights(totalQty, weights, records);
        if (result.status != Status.OK) {
            System.err.println("allocation failed: " + WireTypes.statusToString(result.status));
            return 1;
        }
        System.out.println("Pro-rata allocation of " + totalQty + " units:");
        for (ProRataAllocator.ProRataSlice slice : result.slices) {
            System.out.println("  account " + slice.accountId + ": " + slice.qtyMilli);
        }
        System.out.println("Total allocated: " + result.totalAllocatedMilli);
        return 0;
    }

    private static int cmdPipeline(String[] args) {
        if (args.length < 2) return 1;
        byte[] data = readFile(args[1]);
        if (data == null) return 1;
        Status st = PipelineOrchestrator.runAllocationPipeline(data);
        System.out.println("pipeline: " + WireTypes.statusToString(st));
        return st == Status.OK ? 0 : 1;
    }

    private static int cmdRouter(String[] args) {
        if (args.length < 2) return 1;
        byte[] data = readFile(args[1]);
        if (data == null) return 1;
        Status st = PipelineOrchestrator.runRouterPipeline(data);
        System.out.println("router: " + WireTypes.statusToString(st));
        return st == Status.OK ? 0 : 1;
    }

    private static int cmdMerge(String[] args) {
        if (args.length < 2) return 1;
        byte[] data = readFile(args[1]);
        if (data == null) return 1;
        Status st = PipelineOrchestrator.mergeSessionLegs(data);
        System.out.println("merge: " + WireTypes.statusToString(st));
        return st == Status.OK ? 0 : 1;
    }

    private static int cmdStats() {
        PipelineOrchestrator.PipelineStats stats = PipelineOrchestrator.lastPipelineStats();
        System.out.println("batches_processed=" + stats.batchesProcessed);
        System.out.println("envelopes_sealed=" + stats.envelopesSealed);
        System.out.println("sessions_merged=" + stats.sessionsMerged);
        System.out.println("desk_calls=" + stats.deskCalls);
        return 0;
    }

    private static byte[] readFile(String path) {
        try {
            return Files.readAllBytes(Path.of(path));
        } catch (IOException e) {
            System.err.println("cannot read " + path + ": " + e.getMessage());
            return null;
        }
    }

    private static int parseInt(String s, int def) {
        try { return Integer.parseInt(s); } catch (NumberFormatException e) { return def; }
    }
}
