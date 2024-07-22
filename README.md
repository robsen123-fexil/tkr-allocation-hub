# tkr-allocation-hub

Trade allocation and custody desk library for institutional block-trade distribution, margin aggregation, compliance screening, and session checkpointing.

**Author:** misgana tsegaye

## Overview

`tkr-allocation-hub` implements a full trade allocation pipeline:

- Binary batch wire codec (TKR1/TKR2/TKR3 magic family)
- FIX 4.4 session parser and SWIFT MT940 scanner
- Pro-rata allocator with largest-remainder distribution
- Min/max constraint allocation solver
- Compliance rule engine, margin aggregator, position ledger
- Collateral haircut calculator, lot splitter, fee accrual
- Restriction filter, corporate action adjuster, tax lot matcher
- Benchmark tracker with tracking error computation
- Audit spool and checkpoint ledger
- JNI native bridge for deferred digest flush paths (batch / envelope / session)

## Build

### JVM (Gradle, Java 17)

Requires JDK 17 and Gradle 8.x.

```bash
gradle jar test
```

Run the CLI:

```bash
gradle run --args="allocate 1000 40 30 30"
gradle run --args="pipeline batch.bin"
gradle run --args="stats"
```

Compile fuzz harnesses (for local Jazzer use):

```bash
gradle compileFuzz
```

Main class: `com.tkr.tools.TkrCtl`

Native JNI library (optional, for ASAN fuzzing):

```bash
# Linux / ClusterFuzzLite container
$CXX -fPIC -shared -I${JAVA_HOME}/include -I${JAVA_HOME}/include/linux \
  native/tkr_native.cpp -o libtkr_native.so
export LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH
```

Set `-Dtkr.native.path=/path/to/dir` to load `libtkr_native` from a custom directory.

### ClusterFuzzLite (JVM / Jazzer)

```bash
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
export JAZZER_API_PATH=/path/to/jazzer_standalone.jar
export OUT=out
bash .clusterfuzzlite/build.sh
```

Fuzz targets built from `src/fuzz/java/com/tkr/fuzz/`:

| Harness | Entry Point |
|---------|-------------|
| `BatchFuzzer` | `PipelineOrchestrator.runAllocationPipeline` |
| `RouterFuzzer` | `PipelineOrchestrator.runRouterPipeline` |
| `SessionFuzzer` | `PipelineOrchestrator.mergeSessionLegs` |
| `FixFuzzer` | `Fix44SessionParser.parseMessage` |
| `SwiftFuzzer` | `SwiftMt940Scanner.scan` |

The build produces `tkr-allocation-hub.jar`, `libtkr_native.so`, and Jazzer driver scripts under `$OUT`.

### CMake (legacy C++)

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

### Make (legacy C++)

```bash
make
make test
```

## Tests

```bash
gradle test
```

Integration test `ProRataAllocationTest` verifies pro-rata allocation: 1000 shares with 40/30/30 weights produces 400/300/300.

Legacy C++ integration test:

```bash
./build/allocation_test
```

## CLI

```bash
# Pro-rata: 1000 shares across 40/30/30 weights
gradle run --args="allocate 1000 40 30 30"

# Run allocation pipeline on wire batch file
gradle run --args="pipeline batch.bin"

# Show pipeline statistics
gradle run --args="stats"
```

## Module Table

| Module | Path | Responsibility |
|--------|------|----------------|
| Pro-Rata Allocator | `desk/ProRataAllocator` | Largest-remainder share distribution |
| Allocation Solver | `desk/AllocationSolver` | Min/max bound constraint solver |
| Compliance Engine | `desk/ComplianceRuleEngine` | Multi-rule compliance screening |
| Margin Aggregator | `desk/MarginAggregator` | Account-level margin roll-up with SPAN scan |
| Position Ledger | `desk/PositionLedger` | Double-entry position book |
| Haircut Calculator | `desk/HaircutCalculator` | Tiered collateral haircut grid |
| Lot Splitter | `desk/LotSplitter` | Round/odd lot splitting policies |
| Fee Accrual Engine | `desk/FeeAccrualEngine` | Tiered fee schedule accrual |
| Restriction Filter | `desk/RestrictionFilter` | Trading restriction screening |
| Corporate Action Adjuster | `desk/CorporateActionAdjuster` | Split/dividend/merger adjustments |
| Tax Lot Matcher | `desk/TaxLotMatcher` | FIFO/LIFO/HIFO tax lot matching |
| Benchmark Tracker | `desk/BenchmarkTracker` | Active weight and tracking error |
| Batch Wire Codec | `wire/BatchWireCodec` | TKR1 binary batch encode/decode |
| FIX 4.4 Parser | `wire/Fix44SessionParser` | Stateful FIX session parsing |
| SWIFT MT940 Scanner | `wire/SwiftMt940Scanner` | MT940 statement field parsing |
| Pipeline Orchestrator | `engine/PipelineOrchestrator` | Batch/router/session pipelines |
| Native Bridge | `nativelink/NativeBridge` | JNI digest flush to libtkr_native |
| Audit Spool | `ledger/AuditSpool` | Append-only audit event chain |
| Checkpoint Ledger | `ledger/CheckpointLedger` | Session checkpoint sealing |

## Wire Formats

See [docs/FORMAT.md](docs/FORMAT.md) for TKR1 batch wire, envelope, and session frame layouts.

## License

Proprietary. All rights reserved.
