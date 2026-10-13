#!/usr/bin/env python3
"""Summarise bench/results/raw.jsonl -> bench/results.json (median/min/max of runs)."""
import json, statistics as st, pathlib
R = pathlib.Path("bench/results")
rows = [json.loads(l) for l in open(R / "raw.jsonl")]
stats = json.load(open(R / "stats.json"))
def agg(vals): return {"median": st.median(vals), "min": min(vals), "max": max(vals), "runs": vals}
out = {"data": {"file": "NASDAQ TotalView-ITCH 5.0 sample, 30 Jan 2019 (first 400 MiB of gzip = slice)",
                "messages": stats["messages"], "bytes": stats["bytes"],
                "distinct_stock_locates": stats["distinct_locates"], "locates_with_orders": stats["locates_with_orders"],
                "first_ts_ns_since_midnight": stats["first_ts_ns"], "last_ts_ns_since_midnight": stats["last_ts_ns"],
                "type_counts": stats["type_counts"]},
       "environment": open(R / "environment.txt").read(), "books": {}}
for bk in ("fast", "map"):
    t = [r for r in rows if r["book"] == bk and r["mode"] == "throughput"]
    l = [r for r in rows if r["book"] == bk and r["mode"] == "latency"]
    out["books"][bk] = {
        "throughput_msgs_per_sec": agg([r["msgs_per_sec"] for r in t]),
        "throughput_seconds": agg([r["seconds"] for r in t]),
        "heap_allocs_throughput_run": agg([r["heap_allocs"] for r in t]),
        **{k: agg([r[k] for r in l]) for k in ("p50_ns", "p90_ns", "p99_ns", "p999_ns", "max_ns", "mean_ns", "heap_allocs_after_warmup")},
        "timer_overhead_cycles": [r["timer_overhead_cycles"] for r in l], "tsc_per_ns": [r["tsc_per_ns"] for r in l]}
f, m = out["books"]["fast"], out["books"]["map"]
out["speedup_vs_std_map"] = {
    "throughput": m["throughput_seconds"]["median"] / f["throughput_seconds"]["median"],
    "p50": m["p50_ns"]["median"] / f["p50_ns"]["median"],
    "p99": m["p99_ns"]["median"] / f["p99_ns"]["median"],
    "p999": m["p999_ns"]["median"] / f["p999_ns"]["median"]}
json.dump(out, open("bench/results.json", "w"), indent=2)
print(json.dumps({k: out[k] for k in ("speedup_vs_std_map",)}, indent=2))
for bk in ("fast", "map"):
    b = out["books"][bk]; print(bk, {k: b[k]["median"] for k in ("throughput_msgs_per_sec", "p50_ns", "p99_ns", "p999_ns", "max_ns", "mean_ns", "heap_allocs_after_warmup")})
