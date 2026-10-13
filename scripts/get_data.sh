#!/usr/bin/env bash
# Downloads a slice of a real NASDAQ TotalView-ITCH 5.0 sample day from
# emi.nasdaq.com (public sample files). The full 30 Jan 2019 file is ~4.7 GB
# gzip; by default we fetch the first 400 MiB of the gzip stream and
# decompress what is complete (the replay tool ignores the truncated tail).
#   SLICE_BYTES=0 ./scripts/get_data.sh   # full day
set -euo pipefail
DATE=${DATE:-01302019}
SLICE_BYTES=${SLICE_BYTES:-419430400}
OUT=${OUT:-data}
mkdir -p "$OUT"
URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/${DATE}.NASDAQ_ITCH50.gz"
if [ "$SLICE_BYTES" = 0 ]; then curl -fL -o "$OUT/$DATE.gz" "$URL"
else curl -fL -r 0-$((SLICE_BYTES-1)) -o "$OUT/$DATE.gz" "$URL"; fi
zcat "$OUT/$DATE.gz" 2>/dev/null > "$OUT/$DATE.itch" || true   # truncated gzip -> partial stream
ls -l "$OUT"
