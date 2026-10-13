#!/usr/bin/env bash
# Runs every benchmark RUNS times, pinned to one core, writes raw JSON lines
# and latency histograms to bench/results/, then summarises to bench/results.json.
set -euo pipefail
DATA=${1:?usage: bench/run_all.sh path/to/file.itch}
RUNS=${RUNS:-5}; CPU=${CPU:-2}
B=build/itch_replay; R=bench/results
mkdir -p $R; : > $R/raw.jsonl
{
  echo "date: $(date -Is)"; lscpu | grep -E 'Model name|^CPU\(s\)|Thread|Hypervisor|MHz|L1d|L2|L3'
  uname -r; g++ --version | head -1; echo "flags: -O3 -march=native -std=c++20"; echo "pinned: taskset -c $CPU"
  echo "data: $DATA ($(stat -c %s "$DATA") bytes)"
} > $R/environment.txt
taskset -c $CPU $B "$DATA" --mode stats > $R/stats.json
for i in $(seq 1 $RUNS); do
  for bk in fast map; do
    taskset -c $CPU $B "$DATA" --book $bk --mode throughput --json >> $R/raw.jsonl
    taskset -c $CPU $B "$DATA" --book $bk --mode latency --json --hist $R/hist_${bk}_run$i.csv >> $R/raw.jsonl
  done
done
python3 bench/summarize.py
