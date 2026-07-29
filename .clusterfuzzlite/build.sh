#!/bin/bash -eu

# C++ libFuzzer + ASAN build for tkr-allocation-hub (ClusterFuzzLite / Fenrir)

TKR_ROOT="${SRC:-.}/legacy-cpp"
TKR_INC="-I${TKR_ROOT}/include"
TKR_SRCS=$(find "${TKR_ROOT}/src" -name '*.cc' | sort)

build_fuzzer() {
  local out_name="$1"
  local fuzz_cc="$2"
  $CXX $CXXFLAGS $LIB_FUZZING_ENGINE \
    "${TKR_ROOT}/fuzz/${fuzz_cc}" \
    ${TKR_SRCS} \
    ${TKR_INC} \
    -std=c++17 \
    -o "${OUT}/${out_name}"
}

build_fuzzer BatchFuzzer batch_fuzzer.cc
build_fuzzer RouterFuzzer router_fuzzer.cc
build_fuzzer SessionFuzzer session_fuzzer.cc
build_fuzzer FixFuzzer fix_fuzzer.cc
build_fuzzer SwiftFuzzer swift_fuzzer.cc

echo "C++ libFuzzer build complete -> ${OUT}"
