#!/usr/bin/env python3
"""verify_traceability.py — every decimal number printed in a group README must be
viewable in that group's own CSVs: either verbatim (incremental / baselines /
contention / verdict_probe / per-treatment bench.csv) or via a DOCUMENTED derivation
(pooled median/min/max/σ/margin, var = base+Δ, per-op = per-iter ÷ N, contention
margin from *_min/_max, ldar−ldapr gap, probe Δ×N). Reports any number that cannot
be traced — i.e. a value that exists only in the report. Env constants are allowlisted."""
import os, re, csv, statistics as st, sys
from bisect import bisect_left

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GROUPS = ["1_store_side","2_load_side","3_contention","4_atomics"]
# not results: clock GHz, ns/cyc, mux gate, gcc, kernel, paper §labels
ALLOW = {"3.375","0.296","0.999","11.4","6.8","4.1","4.4","4.5"}

def load(p): return list(csv.DictReader(open(p))) if os.path.isfile(p) else []
def fl(x):
    try: return float(x)
    except: return None

def pool(g):
    vals=set()
    def add(v):
        if v is not None and v==v: vals.add(round(v,6)); vals.add(round(abs(v),6))
    inc=load(f"{REPO}/{g}/processed/{g}_incremental.csv")
    for r in inc:
        n=fl(r.get("stores"))
        for v in r.values():
            x=fl(v); add(x)
            if x is not None and n: add(x/n)
    bro=load(f"{REPO}/{g}/processed/{g}_baselines.csv")
    cells={}
    for r in bro:
        for col in ("base_cyc_iter","base_ns_iter","base_cyc_op"):
            x=fl(r.get(col)); add(x)
            if x is not None: cells.setdefault((r["condition"],r["N"],col),[]).append(x)
    for (c,N,col),vs in cells.items():
        med=st.median(vs); add(med); add(min(vs)); add(max(vs))
        add(st.pstdev(vs) if len(vs)>1 else 0.0)
        add(max(max(vs)-med, med-min(vs)))
        n=fl(N)
        if n: add(med/n)
    for r in inc:                                   # var = pooled base + Δ
        for bcol,dcol in (("base_cyc_iter","incr_cyc_iter"),("base_ns_iter","incr_ns_iter")):
            key=(r.get("condition"),r.get("stores"),bcol)
            if key in cells:
                d=fl(r.get(dcol))
                if d is not None: add(st.median(cells[key])+d)
    co=load(f"{REPO}/{g}/_contention/out/contention.csv")
    for r in co:
        for v in r.values(): add(fl(v))
        for u in ("cyc","ns"):                      # §5b margin from min/max
            ref=fl(r.get(f"base_{u}_op")); lo=fl(r.get(f"base_{u}_op_min")); hi=fl(r.get(f"base_{u}_op_max"))
            if None not in (ref,lo,hi): add(max(hi-ref,ref-lo))
    byn={}
    for r in co: byn.setdefault((r["name"],r["threads"]),r)
    for T in ("1","2","4","8"):                     # gap on any shared column
        a,p=byn.get(("ldar",T)),byn.get(("ldapr",T))
        if a and p:
            for k in a:
                x,y=fl(a[k]),fl(p[k])
                if x is not None and y is not None: add(x-y)
    for r in load(f"{REPO}/{g}/verdict_probe.csv"):  # Verdict probe + Δ×N (fence cost / iter)
        n=fl(r.get("N"))
        for v in r.values():
            x=fl(v); add(x)
            if x is not None and n: add(x*n)
    for d in sorted(os.listdir(f"{REPO}/{g}")):      # raw per-repeat
        for r in load(f"{REPO}/{g}/{d}/out/bench.csv"):
            it=fl(r.get("iters")); na=fl(r.get("n_access"))
            for v in r.values():
                x=fl(v); add(x)
                if x is not None and it: add(x/it)
                if x is not None and na: add(x/na)
    return sorted(vals)

def main():
    bad=0
    for g in GROUPS:
        pv=pool(g)
        txt=open(f"{REPO}/{g}/README.md").read().replace("−","-")
        toks=sorted(set(re.findall(r"[-+]?\d+\.\d+",txt)))
        miss=[]
        for t in toks:
            if t.lstrip("+-") in ALLOW: continue
            d=len(t.split(".")[1]); v=abs(float(t)); tol=0.5*10**-d+1e-9
            i=bisect_left(pv,v-tol)
            if not (i<len(pv) and pv[i]<=v+tol): miss.append(t)
        print(f"[{g}] {len(toks)} decimal tokens, untraceable: {len(miss)}")
        for t in miss: print("   ✗",t)
        bad+=len(miss)
    print(f"\nTOTAL untraceable: {bad}")
    sys.exit(1 if bad else 0)

if __name__=="__main__": main()
