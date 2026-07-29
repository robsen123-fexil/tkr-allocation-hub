CXX ?= clang++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Iinclude
LDFLAGS ?=

SRC := $(shell find src -name '*.cc' | sort)
OBJ := $(SRC:src/%.cc=build/%.o)

LIB := build/libtkr.a
TKRCTL := build/tkrctl
TEST := build/allocation_test

FUZZERS := batch_fuzzer router_fuzzer session_fuzzer fix_fuzzer swift_fuzzer
FUZZ_OUT := $(addprefix build/,$(FUZZERS))

.PHONY: all clean test fuzz

all: $(LIB) $(TKRCTL) $(TEST)

build/%.o: src/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(LIB): $(OBJ)
	@mkdir -p build
	ar rcs $@ $(OBJ)

$(TKRCTL): tools/tkrctl.cc $(LIB)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@ $(LDFLAGS)

$(TEST): tests/integration/allocation_test.cc $(LIB)
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@ $(LDFLAGS)

fuzz: CXXFLAGS += -fsanitize=fuzzer,address
fuzz: LDFLAGS += -fsanitize=fuzzer,address
fuzz: $(FUZZ_OUT)

build/%: fuzz/%.cc $(LIB)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@ $(LDFLAGS)

test: $(TEST)
	./$(TEST)

clean:
	rm -rf build
