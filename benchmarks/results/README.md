# Archived benchmark results

This directory stores machine-readable raw measurements used by the technical
report.

Each dated `report-benchmarks.json` contains:

- the exact command, UTC timestamp, git commit and dirty-tree flag;
- OS/kernel/CPU/RAM and compiler/runtime versions;
- corpus paths, byte sizes, and SHA-256 hashes;
- one warm-up plus every timed sample;
- the median result used in the report;
- output bytes and compression ratio where applicable.

Regenerate the current result set, TSV plot inputs, and plots from the repository
root:

```bash
make bench-report
```

Validate an archived result against checked-in plot data:

```bash
python3 benchmarks/report/validate_report_data.py \
  benchmarks/results/YYYY-MM-DD/report-benchmarks.json
```

The primary report protocol is seven timed samples after one untimed warm-up,
with the median as the reported statistic. CPU affinity, turbo, and frequency
scaling are recorded as uncontrolled unless a future run explicitly controls
them.
