#!/usr/bin/env python3
"""Robustness check for imbalance_study.py: one prediction per mid-price move.

In the main study every top-of-book event is a test point, so consecutive events
before the same mid move share one target. Here each symbol's afternoon is
sampled only at the first event after each mid change (each target used once).
Same per-symbol logistic model on I1, fitted on the morning.
"""
import json, os, sys
import numpy as np, pandas as pd
from sklearn.metrics import roc_auc_score
sys.path.insert(0, os.path.dirname(__file__))
from imbalance_study import load, features, fit_predict, SPLIT

DATA_DIR = sys.argv[1] if len(sys.argv) > 1 else "data/features"
OUT = sys.argv[2] if len(sys.argv) > 2 else "bench/imbalance/robustness_one_per_move.json"
ys, ps, pers, rows = [], [], [], []
for f in sorted(os.listdir(DATA_DIR)):
    if not f.endswith(".bin"): continue
    df = features(*load(os.path.join(DATA_DIR, f)))
    first_after_move = np.r_[False, np.diff(df["mid"].values) != 0]
    tr = df[(df.ts < SPLIT) & (df.y != 0) & (df.t_next < SPLIT)]
    te = df[(df.ts >= SPLIT) & (df.y != 0) & first_after_move]
    p, _ = fit_predict(tr, te, ["I1"])
    y = (te.y.values > 0).astype(int)
    trp = tr[tr.prev_move != 0]; flip = 1 if (trp.prev_move == trp.y).mean() >= 0.5 else -1
    ys.append(y); ps.append(p); pers.append(((flip * te.prev_move.values) > 0).astype(int))
    rows.append((f[:-4], len(te), float(((p > 0.5) == y).mean())))
y = np.concatenate(ys); p = np.concatenate(ps); q = np.concatenate(pers)
res = {"test_moves": int(len(y)), "acc_I1": float(((p > 0.5) == y).mean()),
       "acc_last_move": float((q == y).mean()), "auc_I1": float(roc_auc_score(y, p)),
       "frac_up": float(y.mean()), "per_symbol": rows}
json.dump(res, open(OUT, "w"), indent=1)
print(json.dumps({k: v for k, v in res.items() if k != "per_symbol"}, indent=1))
