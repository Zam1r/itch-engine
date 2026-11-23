#!/usr/bin/env bash
# Reproduces bench/IMBALANCE_STUDY.md end to end (about 25 min on an 8-vCPU VM,
# most of it the AUC bootstrap). Needs ~16 GB free disk, numpy/pandas/scikit-learn/matplotlib.
set -euo pipefail
PY=${PY:-python3}
[ -f data/01302019.itch ] || SLICE_BYTES=0 ./scripts/get_data.sh   # full day, 4.7 GB gzip
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build -j --target itch_features
./build/itch_features data/01302019.itch --count data/rth_counts.csv
SYMS=$(tail -n +2 data/rth_counts.csv | sort -t, -k2 -nr | head -50 | cut -d, -f1 | paste -sd,)
mkdir -p data/features && rm -f data/features/*.bin
./build/itch_features data/01302019.itch --symbols "$SYMS" --out data/features
BOOT_AUC=${BOOT_AUC:-100} $PY scripts/imbalance_study.py data/features bench/imbalance
$PY scripts/imbalance_robustness.py data/features bench/imbalance/robustness_one_per_move.json
