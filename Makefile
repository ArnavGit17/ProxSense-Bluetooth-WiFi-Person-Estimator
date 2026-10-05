# =============================================================================
# ProxSense Makefile
#
# Targets: all, debug, tests, clean
# Compiler: GCC (or Clang if CC is overridden); standard: C11
# External dependency: libbluetooth-dev for the Bluetooth HCI scanner.
# Wi-Fi uses the Linux nl80211 UAPI headers directly (no libnl dependency).
# Install on Ubuntu: sudo apt-get install libbluetooth-dev
# =============================================================================

# --- Toolchain ---
CC      := gcc
AR      := ar

# --- Project layout ---
SRC_DIR  := src
INC_DIR  := include
TST_DIR  := tests
BLD_DIR  := build

# --- Binary name ---
TARGET := $(BLD_DIR)/proxSense

# --- Source files ---
# All .c files in src/ contribute to the main binary.
SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c, $(BLD_DIR)/%.o, $(SRCS))

# --- Test binaries ---
# Each test_*.c in tests/ is compiled as a standalone binary.
TEST_SRCS := $(wildcard $(TST_DIR)/test_*.c)
TEST_BINS := $(patsubst $(TST_DIR)/test_%.c, $(BLD_DIR)/test_%, $(TEST_SRCS))

# Test binaries need the non-main object files.
# Exclude main.o from the objects linked into tests.
LIB_OBJS := $(filter-out $(BLD_DIR)/main.o, $(OBJS))

# --- Warning flags ---
# Strong warnings catch real bugs.
# Note on -Wpedantic + -Wno-variadic-macros:
#   -Wpedantic enforces strict C11 compliance. However, our logging macros use
#   the ##__VA_ARGS__ GCC extension to support zero-argument calls like
#   PS_LOG_INFO("message"). This extension is supported by GCC, Clang, and ICC
#   on all our target platforms. We suppress only this specific extension
#   warning rather than removing -Wpedantic entirely.
WARN_FLAGS := \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Wno-variadic-macros \
    -Wno-pedantic \
    -Wshadow \
    -Wconversion \
    -Wstrict-prototypes \
    -Wmissing-prototypes \
    -Wdouble-promotion \
    -Wnull-dereference \
    -Wformat=2

# --- Base compiler flags ---
CFLAGS := \
    -std=c11 \
    $(WARN_FLAGS) \
    -I$(INC_DIR) \
    -D_POSIX_C_SOURCE=200809L

# --- Linker flags ---
# -lm is needed for math functions in the estimator interface.
LDFLAGS := -lm

# Bluetooth links against BlueZ; Wi-Fi uses the Linux nl80211 UAPI directly.
LDFLAGS += -lbluetooth

# =============================================================================
# Default target
# =============================================================================
.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJS) | $(BLD_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built: $@"

# --- Compile each source file ---
$(BLD_DIR)/%.o: $(SRC_DIR)/%.c | $(BLD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# --- Create build directory if it does not exist ---
$(BLD_DIR):
	mkdir -p $(BLD_DIR)

# =============================================================================
# Debug target — adds sanitisers and debug symbols
# =============================================================================
.PHONY: debug
debug: CFLAGS += -g3 -O0 -DDEBUG \
    -fsanitize=address,undefined \
    -fno-omit-frame-pointer
debug: LDFLAGS += -fsanitize=address,undefined
debug: $(TARGET)
	@echo "Debug build complete: $(TARGET)"

# =============================================================================
# Test target — build and run all test binaries
# =============================================================================
.PHONY: tests
tests: $(TEST_BINS)
	@echo "--- Running unit tests ---"
	@PASS=0; FAIL=0; \
	for t in $(TEST_BINS); do \
	    echo "Running $$t ..."; \
	    $$t; \
	    if [ $$? -eq 0 ]; then PASS=$$((PASS+1)); else FAIL=$$((FAIL+1)); fi; \
	done; \
	echo "--- Test summary: $$PASS passed, $$FAIL failed ---"; \
	[ $$FAIL -eq 0 ]

# Compile each test binary (links against lib objects, not main.o)
$(BLD_DIR)/test_%: $(TST_DIR)/test_%.c $(LIB_OBJS) | $(BLD_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_OBJS) $(LDFLAGS)

# =============================================================================
# Clean target
# =============================================================================
.PHONY: clean
clean:
	rm -rf $(BLD_DIR)
	@echo "Cleaned build directory."

# =============================================================================
# Phony convenience targets
# =============================================================================
.PHONY: help
help:
	@echo "ProxSense Makefile targets:"
	@echo "  all      — Build proxSense binary (default)"
	@echo "  debug    — Build with debug symbols and sanitisers"
	@echo "  tests    — Build and run unit tests"
	@echo "  clean    — Remove build artefacts"
	@echo "  help     — Show this message"
