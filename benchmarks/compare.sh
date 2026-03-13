#!/bin/bash
# Head-to-head benchmark comparing original pigz vs pigzpp.
# Usage: ./compare.sh [data_file]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PIGZPP="${SCRIPT_DIR}/../build/src/cli/pigzpp"
PIGZ="${SCRIPT_DIR}/../../pigz/pigz"

# Check for required tools
command -v hyperfine >/dev/null 2>&1 || { echo "Install hyperfine: cargo install hyperfine"; exit 1; }

# Generate test data if not provided
TMPDIR="${SCRIPT_DIR}/tmp"
mkdir -p "$TMPDIR"

DATA="${1:-}"
if [ -z "$DATA" ]; then
    DATA="${TMPDIR}/pigzpp_bench_data.txt"
    if [ ! -f "$DATA" ]; then
        echo "Generating 50MB test data..."
        python3 -c "
import random, string
with open('$DATA', 'w') as f:
    for i in range(500000):
        f.write(f'Line {i}: {\"x\" * random.randint(20,100)}\n')
"
    fi
fi

SIZE=$(stat --printf="%s" "$DATA" 2>/dev/null || stat -f%z "$DATA")
echo "Test data: $DATA ($(echo "scale=1; $SIZE / 1048576" | bc) MB)"
echo ""

# Check binaries exist
if [ ! -x "$PIGZ" ]; then
    echo "Building original pigz..."
    (cd "$(dirname "$PIGZ")" && make pigz 2>/dev/null || cmake --build build --target pigz_o 2>/dev/null) || true
fi

[ -x "$PIGZPP" ] || { echo "Build pigzpp first: cd pigzpp && mkdir build && cd build && cmake .. && make -j"; exit 1; }
[ -x "$PIGZ" ] || { echo "Original pigz binary not found at $PIGZ"; exit 1; }

echo "=== Compression Benchmark ==="
hyperfine --warmup 1 --runs 3 \
    --export-csv ${TMPDIR}/pigzpp_comp.csv \
    -n "pigz" "$PIGZ -c -k $DATA > /dev/null" \
    -n "pigzpp" "$PIGZPP -c -k $DATA > /dev/null"

echo ""
echo "=== Decompression Benchmark ==="
# Create compressed files
$PIGZ -c -k "$DATA" > ${TMPDIR}/pigzpp_bench.pigz.gz
$PIGZPP -c -k "$DATA" > ${TMPDIR}/pigzpp_bench.pigzpp.gz

hyperfine --warmup 1 --runs 3 \
    --export-csv ${TMPDIR}/pigzpp_decomp.csv \
    -n "pigz" "$PIGZ -d -c ${TMPDIR}/pigzpp_bench.pigz.gz > /dev/null" \
    -n "pigzpp" "$PIGZPP -d -c ${TMPDIR}/pigzpp_bench.pigzpp.gz > /dev/null"

echo ""
echo "=== Compression Ratio ==="
PIGZ_SIZE=$(stat --printf="%s" ${TMPDIR}/pigzpp_bench.pigz.gz 2>/dev/null || stat -f%z ${TMPDIR}/pigzpp_bench.pigz.gz)
PIGZPP_SIZE=$(stat --printf="%s" ${TMPDIR}/pigzpp_bench.pigzpp.gz 2>/dev/null || stat -f%z ${TMPDIR}/pigzpp_bench.pigzpp.gz)
echo "  pigz:   $(echo "scale=1; $PIGZ_SIZE / 1048576" | bc) MB ($(echo "scale=1; 100 * (1 - $PIGZ_SIZE / $SIZE)" | bc)% reduction)"
echo "  pigzpp: $(echo "scale=1; $PIGZPP_SIZE / 1048576" | bc) MB ($(echo "scale=1; 100 * (1 - $PIGZPP_SIZE / $SIZE)" | bc)% reduction)"

echo ""
echo "Results saved to ${TMPDIR}/pigzpp_comp.csv and ${TMPDIR}/pigzpp_decomp.csv"

# Cleanup
rm -f ${TMPDIR}/pigzpp_bench.pigz.gz ${TMPDIR}/pigzpp_bench.pigzpp.gz
