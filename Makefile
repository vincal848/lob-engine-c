# Build and test the matching engine core.

CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
BUILD ?= build

.PHONY: all test exchange-test replay-test replay replay-bench fetch-lobster bench alloc-test lib python-test asan clean

all: $(BUILD)/unit_tests $(BUILD)/bench $(BUILD)/lobster_replay

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/lob.o: src/lob.c src/lob.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ src/lob.c

$(BUILD)/unit_tests: tests/unit_tests.c src/lob.h $(BUILD)/lob.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/unit_tests.c $(BUILD)/lob.o

# M5a: the exchange state machine, layered on the book.
$(BUILD)/exchange.o: src/exchange.c src/exchange.h src/lob.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ src/exchange.c

$(BUILD)/exchange_tests: tests/exchange_tests.c tests/ex_random.h src/exchange.h src/lob.h $(BUILD)/exchange.o $(BUILD)/lob.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/exchange_tests.c $(BUILD)/exchange.o $(BUILD)/lob.o

exchange-test: $(BUILD)/exchange_tests
	./$(BUILD)/exchange_tests

$(BUILD)/bench: bench/bench.c src/lob.h $(BUILD)/lob.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ bench/bench.c $(BUILD)/lob.o

$(BUILD)/lobster_replay: tools/lobster_replay.c src/lob.h $(BUILD)/lob.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/lobster_replay.c $(BUILD)/lob.o

test: $(BUILD)/unit_tests $(BUILD)/exchange_tests replay-test
	./$(BUILD)/unit_tests
	./$(BUILD)/exchange_tests

# The replay harness against a hand-built two-level day (see
# tests/fixtures/README.md): the good file must match, and a copy with
# one corrupted level must be reported as a mismatch.
FIXTURES = tests/fixtures
replay-test: $(BUILD)/lobster_replay
	./$(BUILD)/lobster_replay -q $(FIXTURES)/replay_message_2.csv $(FIXTURES)/replay_orderbook_2.csv
	! ./$(BUILD)/lobster_replay -q $(FIXTURES)/replay_message_2.csv $(FIXTURES)/replay_orderbook_2_bad.csv > /dev/null 2>&1
	@echo "replay fixtures passed"

# M2 on real data. The LOBSTER sample files are not redistributed
# here; `make fetch-lobster` downloads them from lobsterdata.com into
# data/ (gitignored).
LOBSTER_TICKER ?= AAPL
LOBSTER_LEVELS ?= 10
LOBSTER_STEM = data/$(LOBSTER_TICKER)_2012-06-21_34200000_57600000
LOBSTER_ZIP = LOBSTER_SampleFile_$(LOBSTER_TICKER)_2012-06-21_$(LOBSTER_LEVELS).zip

fetch-lobster:
	mkdir -p data
	curl -sSfL -o data/$(LOBSTER_ZIP) https://php.lobsterdata.com/info/sample/$(LOBSTER_ZIP)
	cd data && unzip -oq $(LOBSTER_ZIP)

replay: $(BUILD)/lobster_replay
	./$(BUILD)/lobster_replay $(LOBSTER_STEM)_message_$(LOBSTER_LEVELS).csv $(LOBSTER_STEM)_orderbook_$(LOBSTER_LEVELS).csv

bench: $(BUILD)/bench
	./$(BUILD)/bench

# M3: fails if lob.o calls the allocator between lob_new and lob_free.
# Uses GNU ld's --wrap, so it lives outside `make test`.
$(BUILD)/alloc_test: tests/alloc_test.c src/lob.h $(BUILD)/lob.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/alloc_test.c $(BUILD)/lob.o \
	    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free

alloc-test: $(BUILD)/alloc_test $(BUILD)/alloc_exchange_test
	./$(BUILD)/alloc_test
	./$(BUILD)/alloc_exchange_test

# M5a: the same check for ex_submit -- the exchange layer may allocate
# in ex_new/ex_add_symbol, never while requests are processed.
$(BUILD)/alloc_exchange_test: tests/alloc_exchange_test.c tests/ex_random.h src/exchange.h src/lob.h $(BUILD)/exchange.o $(BUILD)/lob.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/alloc_exchange_test.c $(BUILD)/exchange.o $(BUILD)/lob.o \
	    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free

# M4: the shared library python/lob.py loads through ctypes.
lib: $(BUILD)/liblob.so

$(BUILD)/liblob.so: src/lob.c src/lob.h | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ src/lob.c

python-test: $(BUILD)/liblob.so
	cd python && LOB_LIB=../$(BUILD)/liblob.so python3 test_lob.py

# M3 on real data: per-message book update latency over the LOBSTER day.
replay-bench: $(BUILD)/lobster_replay
	./$(BUILD)/lobster_replay -q -t $(LOBSTER_STEM)_message_$(LOBSTER_LEVELS).csv $(LOBSTER_STEM)_orderbook_$(LOBSTER_LEVELS).csv

# Separate sanitizer pass: rebuilds into build-asan/ with
# AddressSanitizer and UndefinedBehaviorSanitizer enabled and runs the
# same unit tests (and the replay fixtures) against it.
asan:
	$(MAKE) CC=$(CC) BUILD=build-asan \
	    CFLAGS="-std=c11 -O0 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined" \
	    test

clean:
	rm -rf build build-asan
