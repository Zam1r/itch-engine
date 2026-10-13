#!/usr/bin/env python3
"""Extracts a real AAPL message sequence for the portfolio's order-book replay:
the full-depth book at 09:30:00 ET plus the next N book messages."""
import struct, sys, json, collections
path, sym, N = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else "AAPL"), 600
data = open(path, "rb").read()
off, loc = 0, None
orders = {}; out = []; snap = None; T0 = 34200 * 10**9
while off + 2 <= len(data):
    (ml,) = struct.unpack_from(">H", data, off); p = off + 2; off += 2 + ml
    if off > len(data): break
    t = data[p:p+1]
    if t == b"R" and loc is None and data[p+11:p+19].decode().strip() == sym:
        loc = struct.unpack_from(">H", data, p+1)[0]; continue
    if loc is None or t not in b"AFECXDU" or struct.unpack_from(">H", data, p+1)[0] != loc: continue
    ts = int.from_bytes(data[p+5:p+11], "big")
    if ts >= T0 and snap is None:
        lv = collections.defaultdict(int)
        for r, (s, px, sh) in orders.items(): lv[(s, px)] += sh
        snap = [[s, px, q] for (s, px), q in lv.items()]
    ev = None
    if t in b"AF":
        r, s, sh = struct.unpack_from(">QcI", data, p+11); px = struct.unpack_from(">I", data, p+32)[0]
        orders[r] = (s.decode(), px, sh); ev = ["A", ts, r, s.decode(), sh, px]
    elif t in b"ECX":
        r, sh = struct.unpack_from(">QI", data, p+11)
        if r in orders:
            s, px, q = orders[r]; d = min(sh, q); orders[r] = (s, px, q - d)
            if q - d == 0: del orders[r]
            ev = [t.decode(), ts, r, s, d, px]
    elif t == b"D":
        (r,) = struct.unpack_from(">Q", data, p+11)
        if r in orders: s, px, q = orders.pop(r); ev = ["D", ts, r, s, q, px]
    elif t == b"U":
        o, n, sh, px = struct.unpack_from(">QQII", data, p+11)
        if o in orders: s, opx, q = orders.pop(o); orders[n] = (s, px, sh); ev = ["U", ts, n, s, sh, px, opx, q]
    if snap is not None and ev: out.append(ev)
    if len(out) >= N: break
json.dump({"symbol": sym, "date": "2019-01-30", "start_ts_ns": T0, "snapshot": snap, "events": out}, open(sys.argv[3] if len(sys.argv) > 3 else "sample.json", "w"))
print(sym, loc, len(snap or []), len(out))
