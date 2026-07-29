# tkr-allocation-hub

Trade allocation and custody desk library for institutional block-trade distribution, margin aggregation, compliance screening, and session checkpointing.

**Author:** misgana tsegaye

## Overview

`tkr-allocation-hub` implements a full trade allocation pipeline in **C++17**:

- Binary batch wire codec (TKR1/TKR2/TKR3 magic family)
- FIX 4.4 session parser and SWIFT MT940 scanner
- Pro-rata allocator with largest-remainder distribution
- Min/max constraint allocation solver
- Compliance rule engine, margin aggregator, position ledger
- Collateral haircut calculator, lot splitter, fee accrual
- NAV calculator, exposure aggregator, portfolio snapshot builder
- Audit spool and checkpoint ledger
- libFuzzer harnesses with AddressSanitizer for deferred digest UAF paths

A JVM port (Java 17 + Jazzer) remains under `src/main/java/` for reference but **Fenrir builds use the C++ tree in `legacy-cpp/`**.

## Build

### CMake (C++17)

```bash
cd legacy-cpp
mkdir build && cd build
cmake ..
cmake --build .
```

### ClusterFuzzLite / Fenrir (C++ libFuzzer + ASAN)

```bash
export OUT=out
bash .clusterfuzzlite/build.sh
```

Fuzz targets built from `legacy-cpp/fuzz/`:

| Harness | Entry Point |
|---------|-------------|
| `BatchFuzzer` | `RunAllocationPipeline` |
| `RouterFuzzer` | `RunRouterPipeline` |
| `SessionFuzzer` | `MergeSessionLegs` |
| `FixFuzzer` | `Fix44SessionParser::ParseWithSession` |
| `SwiftFuzzer` | `SwiftMt940Scanner::Scan` |

Verify PoCs (Linux):

```bash
./out/BatchFuzzer batch_uaf.bin
./out/RouterFuzzer envelope_uaf.bin
./out/SessionFuzzer session_uaf.bin
```

### JVM (optional reference build)

```bash
gradle jar test
```

## Tests

```bash
cd legacy-cpp/build && ctest
```

Or Gradle for Java unit tests:

```bash
gradle test
```

## Wire Formats

See [docs/FORMAT.md](docs/FORMAT.md) for TKR1 batch wire, envelope, and session frame layouts.

## License

Proprietary. All rights reserved.
