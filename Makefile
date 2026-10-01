# Build and test the matching engine core.

CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
BUILD ?= build

.PHONY: all test bench asan clean

all: $(BUILD)/unit_tests $(BUILD)/bench

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/lob.o: src/lob.c src/lob.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ src/lob.c

$(BUILD)/unit_tests: tests/unit_tests.c src/lob.h $(BUILD)/lob.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/unit_tests.c $(BUILD)/lob.o

$(BUILD)/bench: bench/bench.c src/lob.h $(BUILD)/lob.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ bench/bench.c $(BUILD)/lob.o

test: $(BUILD)/unit_tests
	./$(BUILD)/unit_tests

bench: $(BUILD)/bench
	./$(BUILD)/bench

# Separate sanitizer pass: rebuilds into build-asan/ with
# AddressSanitizer and UndefinedBehaviorSanitizer enabled and runs the
# same unit tests against it.
asan:
	$(MAKE) CC=$(CC) BUILD=build-asan \
	    CFLAGS="-std=c11 -O0 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined" \
	    test

clean:
	rm -rf build build-asan
