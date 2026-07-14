# pigzpp Makefile — common build, test, and benchmark tasks
#
# Usage:
#   make build          Build release mode (optimized, static CLI)
#   make debug          Build debug mode (sanitizers, no optimizations)
#   make test           Run C++ and Python tests
#   make bench-setup    Create benchmark data files, install Python packages
#   make bench-bin      Benchmark CLI: gzip vs pigz vs pigzpp (requires hyperfine)
#   make bench-py       Benchmark Python: gzip vs zlib-ng vs isal vs pigzpp
#   make bench-png      Benchmark PNG encoding vs Pillow baseline
#   make bench          Run all benchmarks
#   make install-py     Install pigzpp Python package (pip install)
#   make clean          Remove build artifacts

NPROC  ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
SIZES  ?= 16 128 1024 8192
ITERS  ?= 3
THREADS ?= 1 4 8 16

BUILD_DIR       := build
DEBUG_DIR       := build-debug
BENCH_DATA_DIR  := build/bench_data
PIGZPP_BIN      := $(BUILD_DIR)/pigzpp

# ─── Setup ────────────────────────────────────────────────────────────────────

.PHONY: setup

setup:
	@bash scripts/setup.sh

# ─── Build ────────────────────────────────────────────────────────────────────

.PHONY: build debug clean test test-cpp test-py

build:
	@mkdir -p $(BUILD_DIR)
	@if [ ! -f $(BUILD_DIR)/CMakeCache.txt ]; then \
		cd $(BUILD_DIR) && cmake .. \
			-DCMAKE_BUILD_TYPE=Release \
			-DPIGZPP_STATIC=ON \
			-DPIGZPP_LTO=ON; \
	fi
	cmake --build $(BUILD_DIR) -j$(NPROC)
	@echo "\n✓ Release build complete: $(PIGZPP_BIN)"
	@ls -lh $(PIGZPP_BIN) $(BUILD_DIR)/pigzpp.cpython-*.so 2>/dev/null || true

debug:
	@mkdir -p $(DEBUG_DIR)
	@if [ ! -f $(DEBUG_DIR)/CMakeCache.txt ]; then \
		cd $(DEBUG_DIR) && cmake .. \
			-DCMAKE_BUILD_TYPE=Debug \
			-DPIGZPP_STATIC=OFF \
			-DPIGZPP_LTO=OFF; \
	fi
	cmake --build $(DEBUG_DIR) -j$(NPROC)
	@echo "\n✓ Debug build complete: $(DEBUG_DIR)/pigzpp"

clean:
	rm -rf $(BUILD_DIR) $(DEBUG_DIR)

# ─── Tests ────────────────────────────────────────────────────────────────────

test: test-cpp test-py

test-cpp: build
	cd $(BUILD_DIR) && ctest --output-on-failure -j$(NPROC)

test-py: build
	PYTHONPATH=$(CURDIR)/$(BUILD_DIR) python -m pytest tests/test_python.py tests/test_png.py -v

# ─── Benchmarks ───────────────────────────────────────────────────────────────

.PHONY: bench-setup bench-bin bench-py bench-png bench

# Generate test data files and install Python benchmark dependencies
bench-setup:
	python3 benchmarks/core/gen_data.py --sizes $(SIZES) --data-dir $(BENCH_DATA_DIR)
	@echo "\nInstalling Python benchmark packages..."
	pip install --quiet zlib-ng isal pytest 2>/dev/null || \
		echo "Warning: some packages failed to install (optional)"
# sudo apt install isal
	@echo "✓ Benchmark setup complete"

# Binary benchmark: gzip vs pigz vs pigzpp
bench-bin: bench-setup
	python benchmarks/core/bench_binary.py --sizes $(SIZES) --iterations $(ITERS) \
		--threads $(THREADS) --pigzpp $(PIGZPP_BIN) --data-dir $(BENCH_DATA_DIR)

# Python benchmark: gzip vs zlib-ng vs isal vs pigzpp
bench-py: bench-setup
	PYTHONPATH=$(CURDIR)/$(BUILD_DIR) python benchmarks/python/bench_python.py --sizes $(SIZES) --iterations $(ITERS)

# PNG benchmark: pigzpp.png and OpenCV against Pillow baseline
bench-png: build
	PYTHONPATH=$(CURDIR)/$(BUILD_DIR) python benchmarks/png/bench_png.py --verify --out $(BUILD_DIR)/png-bench

# All benchmarks
bench: bench-bin bench-py bench-png

# ─── Python Package ──────────────────────────────────────────────────────────

.PHONY: install-py profile

install-py:
	pip install .
	@echo "✓ pigzpp Python package installed"
	python -c "import pigzpp; print('pigzpp:', dir(pigzpp))"

# ─── Profiling ────────────────────────────────────────────────────────────────

PROFILE_SIZE ?= 1024
PROFILE_DATA := $(BENCH_DATA_DIR)/$(PROFILE_SIZE)MB.txt

profile: build
	@mkdir -p $(BENCH_DATA_DIR)
	@if [ ! -f $(PROFILE_DATA) ]; then \
		python3 benchmarks/core/gen_data.py --sizes $(PROFILE_SIZE) --data-dir $(BENCH_DATA_DIR); \
	fi
	@echo "==> Profiling compression ($(PROFILE_SIZE) MB) ..."
	perf record -g -o build/perf-compress.data -- $(PIGZPP_BIN) -c $(PROFILE_DATA) > /dev/null
	perf report -i build/perf-compress.data --no-children --percent-limit 1 | head -60
	@echo "\n==> Profiling decompression ..."
	$(PIGZPP_BIN) -c $(PROFILE_DATA) > $(BENCH_DATA_DIR)/profile_tmp.gz
	perf record -g -o build/perf-decompress.data -- $(PIGZPP_BIN) -dc $(BENCH_DATA_DIR)/profile_tmp.gz > /dev/null
	perf report -i build/perf-decompress.data --no-children --percent-limit 1 | head -60
	@rm -f $(BENCH_DATA_DIR)/profile_tmp.gz
	@echo "\n✓ Profiles saved: build/perf-compress.data, build/perf-decompress.data"
