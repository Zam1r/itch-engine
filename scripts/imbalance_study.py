#!/usr/bin/env python3
"""Order-book imbalance -> next mid-price move: out-of-sample study.

Input: per-symbol top-of-book event files written by `itch_features`
(64-byte records, see src/features_main.cpp), one file per symbol in DATA_DIR.

Method (all chronological, no look-ahead):
  * events = changes of best bid/ask price or size during 09:30-16:00 ET;
    several events with the same nanosecond timestamp are collapsed to the last
    state (intermediate states inside one timestamp are not observable).
  * target: sign of the NEXT non-zero mid-price change (event time).
  * features: L1 imbalance I1=(Qb-Qa)/(Qb+Qa); cumulative depth imbalance over
    the top k=2..5 levels; Cont-Kukanov-Stoikov order-flow imbalance (OFI)
    summed over the last 10 events and scaled by current L1 depth; the sign of
    the previous mid change.
  * train = 09:30-12:30, test = 12:30-16:00. Train events whose next mid change
    happens after 12:30 are dropped. Models are fitted per symbol.
  * baselines: majority class of the training set; last-move rule (next move =
    previous move, or its reverse if that was more accurate on the training
    set; called "persistence" in the code).
  * uncertainty: moving-block bootstrap over 1-minute blocks of the test
    afternoon (all symbols resampled jointly), 95% percentile intervals.
  * OFI regression (Cont, Kukanov & Stoikov 2014): per symbol, over 10-second
    buckets, regress mid change (ticks) on OFI (contemporaneous) and on the
    previous bucket's OFI (predictive); out-of-sample R^2 on the afternoon,
    benchmarked against the training-mean forecast.
Writes bench/imbalance/results.json, tables and PNG figures.
"""
import json, sys, os, time
import numpy as np, pandas as pd
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import roc_auc_score

DATA_DIR = sys.argv[1] if len(sys.argv) > 1 else "data/features"
OUT_DIR = sys.argv[2] if len(sys.argv) > 2 else "bench/imbalance"
os.makedirs(OUT_DIR, exist_ok=True)
NS = 1_000_000_000
OPEN, SPLIT, CLOSE = 34200 * NS, 45000 * NS, 57600 * NS   # 09:30, 12:30, 16:00 ET
OFI_WIN = 10
BOOT = int(os.environ.get("BOOT", 1000))
SEED = 20190130

REC = np.dtype([("ts", "<u8"), ("sym", "<u2"), ("p0", "<u2"), ("p1", "<u4"),
                ("bp", "<u4"), ("ap", "<u4"), ("bq", "<u4", 5), ("aq", "<u4", 5)])
assert REC.itemsize == 64

def load(path):
    r = np.fromfile(path, dtype=REC)
    # collapse same-timestamp events to the last state
    last = np.r_[r["ts"][1:] != r["ts"][:-1], True]
    r = r[last]
    ok = (r["bp"] > 0) & (r["ap"] > r["bp"]) & (r["bq"][:, 0] > 0) & (r["aq"][:, 0] > 0)
    return r, ok

def features(r, ok):
    """Returns DataFrame of valid events with features and target."""
    r = r[ok]
    ts = r["ts"].astype(np.int64)
    bp = r["bp"].astype(np.float64); ap = r["ap"].astype(np.float64)
    bq = r["bq"].astype(np.float64); aq = r["aq"].astype(np.float64)
    mid = (bp + ap) / 2.0
    n = len(r)
    f = {"ts": ts, "mid": mid, "spread_ticks": (ap - bp) / 100.0}
    f["I1"] = (bq[:, 0] - aq[:, 0]) / (bq[:, 0] + aq[:, 0])
    for k in range(2, 6):
        cb = bq[:, :k].sum(1); ca = aq[:, :k].sum(1)
        f[f"I{k}c"] = (cb - ca) / (cb + ca)
    # Cont-Kukanov-Stoikov event OFI
    e = np.zeros(n)
    pb, pa, qb, qa = bp[:-1], ap[:-1], bq[:-1, 0], aq[:-1, 0]
    cb_, ca_, qb_, qa_ = bp[1:], ap[1:], bq[1:, 0], aq[1:, 0]
    e[1:] = (cb_ >= pb) * qb_ - (cb_ <= pb) * qb - (ca_ <= pa) * qa_ + (ca_ >= pa) * qa
    f["e"] = e
    cs = np.cumsum(e)
    ofi_w = cs - np.r_[np.zeros(OFI_WIN), cs[:-OFI_WIN]] if n > OFI_WIN else cs
    f["OFI10n"] = ofi_w / (bq[:, 0] + aq[:, 0])
    # next / previous non-zero mid change
    dm = np.diff(mid)
    chg = np.nonzero(dm)[0]                     # change between event i and i+1
    nxt = np.searchsorted(chg, np.arange(n), side="left")   # first change index >= i
    has_next = nxt < len(chg)
    j = np.where(has_next, chg[np.minimum(nxt, len(chg) - 1)], 0)
    y = np.where(has_next, np.sign(dm[j]), 0)
    t_next = np.where(has_next, ts[np.minimum(j + 1, n - 1)], np.iinfo(np.int64).max)
    prv = np.searchsorted(chg, np.arange(n), side="left") - 1   # last change strictly before i (between k,k+1 with k+1<=i)
    has_prev = prv >= 0
    f["prev_move"] = np.where(has_prev, np.sign(dm[chg[np.maximum(prv, 0)]]), 0)
    f["y"] = y; f["t_next"] = t_next
    return pd.DataFrame(f)

FEATS_A = ["I1"]
FEATS_B = ["I1", "I2c", "I3c", "I4c", "I5c", "OFI10n", "prev_move"]

def fit_predict(tr, te, feats):
    m = LogisticRegression(C=1.0, max_iter=1000)
    m.fit(tr[feats].values, (tr["y"] > 0).astype(int).values)
    return m.predict_proba(te[feats].values)[:, 1], m

def ofi_regression(df, bucket_s=10):
    b = (df["ts"].values // (bucket_s * NS)).astype(np.int64)
    g = pd.DataFrame({"b": b, "e": df["e"].values, "mid": df["mid"].values})
    agg = g.groupby("b").agg(ofi=("e", "sum"), mid_last=("mid", "last"), mid_first=("mid", "first"))
    allb = np.arange(OPEN // (bucket_s * NS), CLOSE // (bucket_s * NS))
    agg = agg.reindex(allb)
    agg["ofi"] = agg["ofi"].fillna(0.0)
    agg["mid_last"] = agg["mid_last"].ffill()
    dmid = agg["mid_last"].diff() / 100.0          # ticks ($0.01); price units 1e-4 $
    out = pd.DataFrame({"b": allb, "ofi": agg["ofi"].values, "dmid": dmid.values})
    out["ofi_prev"] = out["ofi"].shift(1)
    out = out.dropna()
    tr = out[out["b"] * bucket_s * NS < SPLIT]; te = out[out["b"] * bucket_s * NS >= SPLIT]
    res = {}
    for name, x in [("contemporaneous", "ofi"), ("predictive_next_bucket", "ofi_prev")]:
        X = np.c_[np.ones(len(tr)), tr[x].values]
        beta, *_ = np.linalg.lstsq(X, tr["dmid"].values, rcond=None)
        pred = beta[0] + beta[1] * te[x].values
        sse = ((te["dmid"].values - pred) ** 2).sum()
        sst = ((te["dmid"].values - tr["dmid"].mean()) ** 2).sum()
        X2 = np.c_[np.ones(len(tr)), tr[x].values]
        res[name] = {"beta_ticks_per_share": float(beta[1]), "r2_oos": float(1 - sse / sst),
                     "n_train": int(len(tr)), "n_test": int(len(te))}
    return res

def main():
    t0 = time.time()
    syms = sorted(f[:-4] for f in os.listdir(DATA_DIR) if f.endswith(".bin"))
    rows, tests, regs = [], [], {}
    for s in syms:
        r, ok = load(os.path.join(DATA_DIR, s + ".bin"))
        df = features(r, ok)
        tr = df[(df.ts < SPLIT) & (df.y != 0) & (df.t_next < SPLIT)]
        te = df[(df.ts >= SPLIT) & (df.y != 0)]
        if len(tr) < 1000 or len(te) < 1000:
            print(f"skip {s}: train {len(tr)} test {len(te)}"); continue
        pA, mA = fit_predict(tr, te, FEATS_A)
        pB, mB = fit_predict(tr, te, FEATS_B)
        yt = (te["y"].values > 0).astype(int)
        maj = int((tr["y"] > 0).mean() >= 0.5)
        # last-move baseline: repeat (or, if training data says so, reverse) the previous move
        trp = tr[tr.prev_move != 0]
        flip = 1 if (trp.prev_move == trp.y).mean() >= 0.5 else -1
        pers = flip * te["prev_move"].values
        t = pd.DataFrame({"sym": s, "ts": te["ts"].values, "y": yt, "I1": te["I1"].values,
                          "pA": pA, "pB": pB, "maj": maj,
                          "pers": np.where(pers == 0, maj, (pers > 0).astype(int)),
                          "spread_ticks": te["spread_ticks"].values})
        tests.append(t)
        regs[s] = ofi_regression(df)
        rows.append({"symbol": s, "events_total": int(len(df)), "train_events": int(len(tr)),
                     "test_events": int(len(te)),
                     "median_spread_ticks": float(np.median(df["spread_ticks"])),
                     "mean_price_usd": float(df["mid"].mean() / 1e4),
                     "coef_I1": float(mA.coef_[0][0]), "last_move_rule": "repeat" if flip == 1 else "reverse",
                     "acc_majority": float((t.maj == t.y).mean()),
                     "acc_persistence": float((t.pers == t.y).mean()),
                     "acc_I1": float(((t.pA > 0.5) == t.y).mean()),
                     "acc_full": float(((t.pB > 0.5) == t.y).mean()),
                     "auc_I1": float(roc_auc_score(t.y, t.pA)),
                     "auc_full": float(roc_auc_score(t.y, t.pB)),
                     "ofi_r2_oos_contemp": regs[s]["contemporaneous"]["r2_oos"],
                     "ofi_r2_oos_pred": regs[s]["predictive_next_bucket"]["r2_oos"]})
        print(f"{s}: test={len(te)} accI1={rows[-1]['acc_I1']:.4f} aucI1={rows[-1]['auc_I1']:.4f} ({time.time()-t0:.0f}s)", flush=True)
    per = pd.DataFrame(rows).sort_values("test_events", ascending=False)
    per.to_csv(os.path.join(OUT_DIR, "per_symbol.csv"), index=False, float_format="%.5f")
    T = pd.concat(tests, ignore_index=True)
    T["cA"] = ((T.pA > 0.5) == T.y).astype(np.int8); T["cB"] = ((T.pB > 0.5) == T.y).astype(np.int8)
    T["cM"] = (T.maj == T.y).astype(np.int8); T["cP"] = (T.pers == T.y).astype(np.int8)
    head = {"acc_majority": T.cM.mean(), "acc_persistence": T.cP.mean(), "acc_I1": T.cA.mean(),
            "acc_full": T.cB.mean(), "auc_I1": roc_auc_score(T.y, T.pA), "auc_full": roc_auc_score(T.y, T.pB),
            "test_events": len(T), "symbols": len(per), "train_events": int(per.train_events.sum()),
            "frac_up_test": T.y.mean()}
    # block bootstrap over 1-minute blocks of the afternoon
    rng = np.random.default_rng(SEED)
    T["blk"] = ((T.ts - SPLIT) // (60 * NS)).astype(int)
    nb = int(T.blk.max()) + 1
    g = T.groupby("blk").agg(n=("y", "size"), cA=("cA", "sum"), cB=("cB", "sum"), cM=("cM", "sum"), cP=("cP", "sum"))
    g = g.reindex(range(nb), fill_value=0)
    boots = {k: [] for k in ["acc_I1", "acc_full", "acc_majority", "acc_persistence", "lift_I1_vs_majority", "lift_I1_vs_persistence", "lift_full_vs_persistence"]}
    for _ in range(BOOT):
        idx = rng.integers(0, nb, nb); s = g.iloc[idx].sum()
        a1, aB, aM, aP = s.cA / s.n, s.cB / s.n, s.cM / s.n, s.cP / s.n
        boots["acc_I1"].append(a1); boots["acc_full"].append(aB); boots["acc_majority"].append(aM)
        boots["acc_persistence"].append(aP); boots["lift_I1_vs_majority"].append(a1 - aM)
        boots["lift_I1_vs_persistence"].append(a1 - aP); boots["lift_full_vs_persistence"].append(aB - aP)
    ci = {k: [float(np.percentile(v, 2.5)), float(np.percentile(v, 97.5))] for k, v in boots.items()}
    # AUC bootstrap (fewer reps; on block-resampled index sets)
    blk_rows = [np.nonzero(T.blk.values == b)[0] for b in range(nb)]
    aucA, aucB = [], []
    for _ in range(int(os.environ.get("BOOT_AUC", 200))):
        idx = np.concatenate([blk_rows[b] for b in rng.integers(0, nb, nb)])
        yy = T.y.values[idx]
        aucA.append(roc_auc_score(yy, T.pA.values[idx])); aucB.append(roc_auc_score(yy, T.pB.values[idx]))
    ci["auc_I1"] = [float(np.percentile(aucA, 2.5)), float(np.percentile(aucA, 97.5))]
    ci["auc_full"] = [float(np.percentile(aucB, 2.5)), float(np.percentile(aucB, 97.5))]
    # decile table: I1 deciles (edges from pooled test set) vs empirical P(up)
    T["dec"] = pd.qcut(T.I1, 10, labels=False, duplicates="drop")
    dec = T.groupby("dec").agg(I1_lo=("I1", "min"), I1_hi=("I1", "max"), n=("y", "size"),
                              p_up=("y", "mean"), p_model=("pA", "mean")).reset_index()
    dec["se"] = np.sqrt(dec.p_up * (1 - dec.p_up) / dec.n)
    dec.to_csv(os.path.join(OUT_DIR, "deciles.csv"), index=False, float_format="%.4f")
    # spread buckets
    T["spread_bucket"] = np.where(T.spread_ticks <= 1.0001, "1 tick", "2+ ticks")
    sp = T.groupby("spread_bucket").agg(n=("y", "size"), acc_I1=("cA", "mean"), acc_persistence=("cP", "mean")).reset_index()
    sp.to_csv(os.path.join(OUT_DIR, "by_spread.csv"), index=False, float_format="%.4f")
    # OFI summary
    r2c = per.ofi_r2_oos_contemp; r2p = per.ofi_r2_oos_pred
    ofi = {"bucket_seconds": 10, "median_r2_oos_contemporaneous": float(r2c.median()),
           "mean_r2_oos_contemporaneous": float(r2c.mean()),
           "median_r2_oos_predictive": float(r2p.median()), "mean_r2_oos_predictive": float(r2p.mean()),
           "symbols_r2_contemp_gt_0": int((r2c > 0).sum()), "symbols_r2_pred_gt_0": int((r2p > 0).sum())}
    res = {"headline": {k: float(v) if not isinstance(v, (int, np.integer)) else int(v) for k, v in head.items()},
           "ci95_block_bootstrap": ci, "bootstrap": {"block": "1 minute", "reps_acc": BOOT,
           "reps_auc": int(os.environ.get("BOOT_AUC", 200)), "seed": SEED},
           "symbols_won_vs_persistence_I1": int((per.acc_I1 > per.acc_persistence).sum()),
           "symbols_won_vs_majority_I1": int((per.acc_I1 > per.acc_majority).sum()),
           "ofi_regression": ofi, "ofi_regression_per_symbol": regs, "runtime_s": time.time() - t0}
    json.dump(res, open(os.path.join(OUT_DIR, "results.json"), "w"), indent=2)
    print(json.dumps({k: res[k] for k in ["headline", "ci95_block_bootstrap", "ofi_regression", "symbols_won_vs_persistence_I1", "symbols_won_vs_majority_I1"]}, indent=2))
    print(dec.to_string()); print(sp.to_string())
    # figures
    import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(6.5, 4.2))
    mids = (dec.I1_lo + dec.I1_hi) / 2
    ax.errorbar(range(1, len(dec) + 1), dec.p_up, yerr=1.96 * dec.se, fmt="o-", label="observed P(up), afternoon test set")
    ax.plot(range(1, len(dec) + 1), dec.p_model, "s--", label="logistic model on I1 (fitted on morning)")
    ax.axhline(0.5, color="grey", lw=0.8)
    ax.set_xticks(range(1, len(dec) + 1)); ax.set_xticklabels([f"{a:+.2f}" for a in mids], rotation=45, fontsize=8)
    ax.set_xlabel("L1 imbalance decile (midpoint of I = (Qb-Qa)/(Qb+Qa))"); ax.set_ylabel("P(next mid move is up)")
    ax.set_title(f"NASDAQ ITCH 30 Jan 2019, {len(per)} symbols, {len(T):,} test events", fontsize=9)
    ax.legend(fontsize=8); fig.tight_layout(); fig.savefig(os.path.join(OUT_DIR, "imbalance_deciles.png"), dpi=150)
    fig, ax = plt.subplots(figsize=(8, 4.2))
    p2 = per.sort_values("acc_I1")
    x = np.arange(len(p2))
    ax.bar(x - 0.2, p2.acc_I1, 0.4, label="logistic on I1")
    ax.bar(x + 0.2, p2.acc_persistence, 0.4, label="last-move baseline")
    ax.axhline(0.5, color="grey", lw=0.8)
    ax.set_xticks(x); ax.set_xticklabels(p2.symbol, rotation=90, fontsize=7)
    ax.set_ylim(0.4, max(0.9, p2.acc_I1.max() + 0.02)); ax.set_ylabel("out-of-sample accuracy (12:30-16:00)")
    ax.legend(fontsize=8); fig.tight_layout(); fig.savefig(os.path.join(OUT_DIR, "per_symbol_accuracy.png"), dpi=150)

if __name__ == "__main__":
    main()
