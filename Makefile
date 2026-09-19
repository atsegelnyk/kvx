PROJECT := kv

CC := clang

SRC_DIR := src
TEST_DIR := test
BENCH_DIR := bench
BUILD_DIR := build
VENDOR_DIR := vendor

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

SRC := $(wildcard $(SRC_DIR)/*.c)

# Everything except main.c: the "library" half of the project, linked into
# tests and benchmarks.
LIB_SRC := $(filter-out $(SRC_DIR)/main.c,$(SRC))

VENDOR_SRC := $(VENDOR_DIR)/xxhash/xxhash.c

# One binary per file. Drop a new file in and it builds; no edits here.
TEST_SRC := $(wildcard $(TEST_DIR)/*.c)
BENCH_SRC := $(wildcard $(BENCH_DIR)/*.c)

# ---------------------------------------------------------------------------
# Build mode
# ---------------------------------------------------------------------------

MODE ?= debug

OUT_DIR := $(BUILD_DIR)/$(MODE)
OBJ_DIR := $(OUT_DIR)/obj
BIN_DIR := $(OUT_DIR)/bin

# ---------------------------------------------------------------------------
# Objects
#
# Objects live under obj/, binaries under bin/. Keeping them in separate
# subtrees is what lets the link rules below be plain pattern rules without
# accidentally matching each other's outputs.
# ---------------------------------------------------------------------------

APP_OBJ := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/src/%.o,$(SRC))
LIB_OBJ := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/src/%.o,$(LIB_SRC))

VENDOR_OBJ := $(OBJ_DIR)/vendor/xxhash.o

TEST_OBJ := $(patsubst $(TEST_DIR)/%.c,$(OBJ_DIR)/test/%.o,$(TEST_SRC))
BENCH_OBJ := $(patsubst $(BENCH_DIR)/%.c,$(OBJ_DIR)/bench/%.o,$(BENCH_SRC))

APP_OBJ += $(VENDOR_OBJ)
LIB_OBJ += $(VENDOR_OBJ)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------

TARGET := $(BIN_DIR)/$(PROJECT)

TEST_BINS := $(patsubst $(TEST_DIR)/%.c,$(BIN_DIR)/test/%,$(TEST_SRC))
BENCH_BINS := $(patsubst $(BENCH_DIR)/%.c,$(BIN_DIR)/bench/%,$(BENCH_SRC))

# Run a single target: `make bench BENCH=slab_bench`, `make test TEST=foo_test`.
# Empty means all of them.
BENCH ?=
TEST ?=

BENCH_RUN := $(if $(BENCH),$(BIN_DIR)/bench/$(BENCH),$(BENCH_BINS))
TEST_RUN := $(if $(TEST),$(BIN_DIR)/test/$(TEST),$(TEST_BINS))

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
	-MP \
	-pthread

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

CFLAGS_TEST := \
	-O2 \
	-g \
	-fno-omit-frame-pointer

# -fno-omit-frame-pointer so `perf record -g` can unwind the allocator hot
# path. It costs a register, not measurable time, on x86-64.
CFLAGS_BENCH := \
	-O3 \
	-DNDEBUG \
	-march=native \
	-g \
	-fno-omit-frame-pointer

ifeq ($(MODE),debug)
	CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_DEBUG)
else ifeq ($(MODE),release)
	CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_RELEASE)
else ifeq ($(MODE),sanitize)
	CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_SANITIZE)
else ifeq ($(MODE),test)
	CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_TEST)
else ifeq ($(MODE),bench)
	CFLAGS := $(CFLAGS_COMMON) $(CFLAGS_BENCH)
else
	$(error Unknown MODE "$(MODE)")
endif

LDFLAGS :=
LDLIBS := -pthread -lm

ifeq ($(MODE),sanitize)
	LDFLAGS += -fsanitize=address,undefined
endif

# clock_gettime(), fork(), execv() feature declarations for tests/benchmarks.
HARNESS_CPPFLAGS := $(CPPFLAGS) -D_POSIX_C_SOURCE=200809L

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
# Compilation
# ---------------------------------------------------------------------------

$(OBJ_DIR)/src/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/vendor/xxhash.o: $(VENDOR_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/test/%.o: $(TEST_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HARNESS_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/bench/%.o: $(BENCH_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HARNESS_CPPFLAGS) $(CFLAGS) -c $< -o $@

# ---------------------------------------------------------------------------
# Linking tests and benchmarks
# ---------------------------------------------------------------------------

$(BIN_DIR)/test/%: $(OBJ_DIR)/test/%.o $(LIB_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $< $(LIB_OBJ) $(LDLIBS) -o $@

$(BIN_DIR)/bench/%: $(OBJ_DIR)/bench/%.o $(LIB_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $< $(LIB_OBJ) $(LDLIBS) -o $@

# ---------------------------------------------------------------------------
# Test
#
# The public targets re-enter make with MODE fixed, so the recursive half is
# named with a leading underscore and is never invoked directly.
# ---------------------------------------------------------------------------

.PHONY: _test-build
_test-build: $(TEST_BINS)

.PHONY: test-build
test-build:
	$(MAKE) MODE=test _test-build

.PHONY: test
test:
	$(MAKE) MODE=test _test-build
	@for b in $(patsubst $(TEST_DIR)/%.c,$(BUILD_DIR)/test/bin/test/%,$(TEST_SRC)); do \
		echo "== $$b"; "$$b" || exit 1; \
	done

# Same binaries under ASan/UBSan. The slab bench's integrity pass is worth
# running here: it is what catches overlapping chunks and bad alignment.
.PHONY: test-sanitize
test-sanitize:
	$(MAKE) MODE=sanitize _test-build
	@for b in $(patsubst $(TEST_DIR)/%.c,$(BUILD_DIR)/sanitize/bin/test/%,$(TEST_SRC)); do \
		echo "== $$b"; "$$b" || exit 1; \
	done

# ---------------------------------------------------------------------------
# Benchmark
# ---------------------------------------------------------------------------

.PHONY: _bench-build
_bench-build: $(BENCH_BINS)

.PHONY: bench-build
bench-build:
	$(MAKE) MODE=bench _bench-build

.PHONY: bench
bench:
	$(MAKE) MODE=bench _bench-build
	@for b in $(if $(BENCH),$(BUILD_DIR)/bench/bin/bench/$(BENCH),\
		$(patsubst $(BENCH_DIR)/%.c,$(BUILD_DIR)/bench/bin/bench/%,$(BENCH_SRC))); do \
		echo "== $$b"; "$$b" || exit 1; \
	done

# Benchmarks under ASan measure nothing useful, but they do prove the harness
# itself is clean. Run this once after changing an allocator, not routinely.
.PHONY: bench-sanitize
bench-sanitize:
	$(MAKE) MODE=sanitize _bench-build
	@for b in $(if $(BENCH),$(BUILD_DIR)/sanitize/bin/bench/$(BENCH),\
		$(patsubst $(BENCH_DIR)/%.c,$(BUILD_DIR)/sanitize/bin/bench/%,$(BENCH_SRC))); do \
		echo "== $$b"; "$$b" || exit 1; \
	done

# ---------------------------------------------------------------------------
# Profiling
#
#   make profile BENCH=slab_bench RUN="churn slab fixed 10"
#
# MODE=bench already carries -g -fno-omit-frame-pointer, so `--call-graph fp`
# unwinds correctly and costs far less than dwarf unwinding.
# ---------------------------------------------------------------------------

PROF_BIN := $(BUILD_DIR)/bench/bin/bench/$(BENCH)
RUN ?= churn slab mixed 10

# Last CPU index we are actually allowed to run on. nproc already respects the
# affinity mask, so this stays valid inside a cgroup or a 2-vCPU VPS. Set
# PIN=0 to skip pinning entirely.
PIN ?= 1
PERF_CPU ?= $(shell expr $$(nproc) - 1)
PIN_CMD := $(if $(filter 1,$(PIN)),taskset -c $(PERF_CPU),)

# Most cloud VMs expose no hardware PMU, in which case cycles/cache events are
# unavailable and perf must sample off the timer instead.
HAS_PMU := $(shell test -d /sys/bus/event_source/devices/cpu \
	-o -d /sys/bus/event_source/devices/cpu_core && echo 1)
PERF_EVENT ?= $(if $(HAS_PMU),cycles,cpu-clock)

# A backslash-continuation inside a make variable collapses to a space, which
# perf rejects mid-list, so these stay on single lines.
COMMA := ,
PERF_SW_EVENTS := task-clock,page-faults,context-switches
PERF_HW_EVENTS := cycles,instructions,branch-misses,L1-dcache-loads,L1-dcache-load-misses,dTLB-loads,dTLB-load-misses
PERF_STAT_EVENTS ?= $(if $(HAS_PMU),$(PERF_HW_EVENTS)$(COMMA))$(PERF_SW_EVENTS)

.PHONY: perf-check
perf-check:
	@echo "cpus available : $$(nproc)   pinning to: $(if $(filter 1,$(PIN)),$(PERF_CPU),<none>)"
	@echo "hardware PMU   : $(if $(HAS_PMU),yes,NO - timer sampling only, no cache counters)"
	@echo "sample event   : $(PERF_EVENT)"
	@echo "paranoid level : $$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo '?')"
	@echo
	@echo "if paranoid > 1, run once:  sudo sysctl -w kernel.perf_event_paranoid=1"
	@echo "then use plain 'make profile' - do NOT use sudo, it builds as root."

.PHONY: profile
profile: perf-check
	@test -n "$(BENCH)" || { echo "usage: make profile BENCH=<name> [RUN=...]"; exit 1; }
	$(MAKE) MODE=bench _bench-build
	perf record -F 999 -e $(PERF_EVENT) --call-graph fp -o perf.data \
		-- $(PIN_CMD) ./$(PROF_BIN) --run $(RUN)
	perf report --no-children --percent-limit 0.5 -i perf.data

.PHONY: profile-stat
profile-stat: perf-check
	@test -n "$(BENCH)" || { echo "usage: make profile-stat BENCH=<name> [RUN=...]"; exit 1; }
	$(MAKE) MODE=bench _bench-build
	perf stat -e $(PERF_STAT_EVENTS) -- $(PIN_CMD) ./$(PROF_BIN) --run $(RUN)

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
	./$(BUILD_DIR)/debug/bin/$(PROJECT)

.PHONY: run-release
run-release:
	$(MAKE) MODE=release all
	./$(BUILD_DIR)/release/bin/$(PROJECT)

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
	bear -- $(MAKE) debug test-build bench-build

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

DEP := \
	$(APP_OBJ:.o=.d) \
	$(TEST_OBJ:.o=.d) \
	$(BENCH_OBJ:.o=.d)

-include $(DEP)