PROJECT := kv

CC := clang

SRC_DIR := src
BENCH_DIR := bench
BUILD_DIR := build
VENDOR_DIR := vendor

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

SRC := $(wildcard $(SRC_DIR)/*.c)

# Все исходники кроме main.c.
# Используются как "библиотечная" часть проекта в benchmark.
LIB_SRC := $(filter-out $(SRC_DIR)/main.c,$(SRC))

VENDOR_SRC := $(VENDOR_DIR)/xxhash/xxhash.c

BENCH_SRC := $(BENCH_DIR)/hashtable_bench.c

# ---------------------------------------------------------------------------
# Build mode
# ---------------------------------------------------------------------------

MODE ?= debug

OUT_DIR := $(BUILD_DIR)/$(MODE)

# ---------------------------------------------------------------------------
# Objects
# ---------------------------------------------------------------------------

APP_OBJ := $(patsubst $(SRC_DIR)/%.c,$(OUT_DIR)/src/%.o,$(SRC))

LIB_OBJ := $(patsubst $(SRC_DIR)/%.c,$(OUT_DIR)/src/%.o,$(LIB_SRC))

VENDOR_OBJ := $(OUT_DIR)/vendor/xxhash.o

BENCH_OBJ := $(OUT_DIR)/bench/hashtable_bench.o

APP_OBJ += $(VENDOR_OBJ)
LIB_OBJ += $(VENDOR_OBJ)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------

TARGET := $(OUT_DIR)/$(PROJECT)
BENCH_TARGET := $(OUT_DIR)/hashtable_bench

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------

CPPFLAGS := \
	-I$(SRC_DIR) \
	-I$(VENDOR_DIR)/xxhash

CFLAGS_COMMON := \
	-std=c17 \
	-Wall \
	-Wextra \
	-Wpedantic \
	-Wshadow \
	-Wstrict-prototypes \
	-MMD \
	-MP

CFLAGS_DEBUG := \
	-O0 \
	-g3

CFLAGS_RELEASE := \
	-O3 \
	-DNDEBUG

CFLAGS_SANITIZE := \
	-O1 \
	-g3 \
	-fsanitize=address,undefined \
	-fno-omit-frame-pointer

CFLAGS_BENCH := \
	-O3 \
	-DNDEBUG \
	-march=native

ifeq ($(MODE),debug)
	CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_DEBUG)
else ifeq ($(MODE),release)
	CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_RELEASE)
else ifeq ($(MODE),sanitize)
	CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_SANITIZE)
else ifeq ($(MODE),bench)
	CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_BENCH)
else
	$(error Unknown MODE "$(MODE)")
endif

LDFLAGS :=
LDLIBS :=

ifeq ($(MODE),sanitize)
	LDFLAGS += -fsanitize=address,undefined
endif

# clock_gettime() feature declarations for benchmark.
BENCH_CPPFLAGS := $(CPPFLAGS) -D_POSIX_C_SOURCE=200809L

# ---------------------------------------------------------------------------
# Default build
# ---------------------------------------------------------------------------

.PHONY: all
all: $(TARGET)

# ---------------------------------------------------------------------------
# Application linking
# ---------------------------------------------------------------------------

$(TARGET): $(APP_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(APP_OBJ) $(LDLIBS) -o $@

# ---------------------------------------------------------------------------
# src/*.c
# ---------------------------------------------------------------------------

$(OUT_DIR)/src/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# ---------------------------------------------------------------------------
# Vendor
# ---------------------------------------------------------------------------

$(OUT_DIR)/vendor/xxhash.o: $(VENDOR_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# ---------------------------------------------------------------------------
# Benchmark
# ---------------------------------------------------------------------------

$(BENCH_OBJ): $(BENCH_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(BENCH_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BENCH_TARGET): $(BENCH_OBJ) $(LIB_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(BENCH_OBJ) $(LIB_OBJ) $(LDLIBS) -o $@

.PHONY: _bench-build
_bench-build: $(BENCH_TARGET)

.PHONY: bench-build
bench-build:
	$(MAKE) MODE=bench _bench-build

.PHONY: bench
bench:
	$(MAKE) MODE=bench _bench-build
	./$(BUILD_DIR)/bench/hashtable_bench

# ---------------------------------------------------------------------------
# Build modes
# ---------------------------------------------------------------------------

.PHONY: debug
debug:
	$(MAKE) MODE=debug all

.PHONY: release
release:
	$(MAKE) MODE=release all

.PHONY: sanitize
sanitize:
	$(MAKE) MODE=sanitize all

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

.PHONY: run
run:
	$(MAKE) MODE=debug all
	./$(BUILD_DIR)/debug/$(PROJECT)

.PHONY: run-release
run-release:
	$(MAKE) MODE=release all
	./$(BUILD_DIR)/release/$(PROJECT)

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

.PHONY: rebuild
rebuild:
	$(MAKE) clean
	$(MAKE) debug

# ---------------------------------------------------------------------------
# compile_commands.json
# ---------------------------------------------------------------------------

.PHONY: compdb
compdb:
	@command -v bear >/dev/null 2>&1 || \
		{ echo "error: bear is not installed"; exit 1; }
	rm -f compile_commands.json
	$(MAKE) clean
	bear -- $(MAKE) debug bench-build

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

DEP := \
	$(APP_OBJ:.o=.d) \
	$(BENCH_OBJ:.o=.d)

-include $(DEP)