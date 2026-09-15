#   make            build everything 
#   make test       build and run the test suite
#   make bench      build and run the benchmark
#   make demo       build and run the walkthrough
#   make tsan       run the tests under ThreadSanitizer
#   make asan       run the tests under AddressSanitizer + UBSan
#   make clean      remove build output

CXX      ?= g++
BUILD    ?= release
CXXFLAGS ?= -std=c++17 -Wall -Wextra -pthread -Iinc

ifeq ($(BUILD),release)
  MODEFLAGS := -O3 -DNDEBUG
else ifeq ($(BUILD),tsan)
  MODEFLAGS := -O1 -g -fsanitize=thread
else ifeq ($(BUILD),asan)
  MODEFLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
else
  $(error unknown BUILD=$(BUILD); use release, tsan or asan)
endif

BIN       := build/$(BUILD)
HEADERS   := $(wildcard inc/*.hpp)
TEST_SRCS := tests/main.cpp tests/test_kv_store.cpp tests/test_sync.cpp
TARGETS   := $(BIN)/kv_tests $(BIN)/kv_demo $(BIN)/kv_bench

# Sanitizer builds trip over ASLR on some kernels; re-run them with it disabled.
SETARCH := $(shell command -v setarch 2>/dev/null)
ifdef SETARCH
  NOASLR := setarch $(shell uname -m) -R
endif

.PHONY: all test demo bench tsan asan clean
all: $(TARGETS)

$(BIN):
	@mkdir -p $(BIN)

$(BIN)/kv_tests: $(TEST_SRCS) $(HEADERS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(MODEFLAGS) -Itests $(TEST_SRCS) -o $@

$(BIN)/kv_demo: src/demo.cpp $(HEADERS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(MODEFLAGS) src/demo.cpp -o $@

$(BIN)/kv_bench: bench/bench.cpp $(HEADERS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(MODEFLAGS) bench/bench.cpp -o $@

test: $(BIN)/kv_tests
	./$(BIN)/kv_tests

demo: $(BIN)/kv_demo
	./$(BIN)/kv_demo

bench: $(BIN)/kv_bench
	./$(BIN)/kv_bench

tsan:
	@$(MAKE) --no-print-directory BUILD=tsan build/tsan/kv_tests
	$(NOASLR) ./build/tsan/kv_tests

asan:
	@$(MAKE) --no-print-directory BUILD=asan build/asan/kv_tests
	$(NOASLR) ./build/asan/kv_tests

clean:
	rm -rf build
