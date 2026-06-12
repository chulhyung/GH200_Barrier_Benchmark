#!/usr/bin/env python3
"""verify_readme_vs_csv.py — cross-check every numeric cell in each group README
against the source CSVs. DIRECT values (Δ from incremental.csv; base/treat/Δ/l1/remote
from contention.csv) are compared cell-for-cell; DERIVED values (pooled-baseline ref
median, margin, var=base+Δ) are recomputed independently. Section-aware single pass, so
it works for flat groups (G1/G2/G3, sections at H2) AND the two-part group (G4: Part A/B
at H2, sections at H3, treatments at H4)."""
import os, re, csv, statistics as stats, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GROUPS = ["1_store_side","2_load_side","3_contention","4_atomics","5_release_serialization"]
NS = [1,2,4,8,16,32,64]
ST_PLACE = {"after_group","after_every","acquire","release","acqrel","seqcst"}
COND_RE = r"(miss|hit|l1|l2|l3|dram)"   # hit/miss (G2/G4) + Group-1 cache-residency levels

def num(s):
    s = s.strip().replace("**","").replace("*","").replace("≈0","").replace("–","-")
    try: return float(s)
    except: return None
def close(a,b,tol):
    if a is None or b is None: return a is None and b is None
    return abs(a-b) <= tol
def load_csv(p): return list(csv.DictReader(open(p))) if os.path.isfile(p) else []
def load_incr(g): return load_csv(os.path.join(REPO,g,"processed",f"{g}_incremental.csv"))
def load_baselines(g): return load_csv(os.path.join(REPO,g,"processed",f"{g}_baselines.csv"))
def load_cont(g): return load_csv(os.path.join(REPO,g,"_contention","out","contention.csv"))
def incr_lookup(rows,t,place,cond,N,col):
    for r in rows:
        if r["treatment"]==t and r["placement"]==place and r["condition"]==cond and int(r["stores"])==N:
            try: return float(r[col])
            except: return None
    return None
def base_pool(brows,cond,N,col):
    return [float(r[col]) for r in brows if r["condition"]==cond and int(r["N"])==N and r.get(col,"")!=""]
def split_cells(line): return [c.strip() for c in line.strip().strip("|").split("|")]
def htext(ln): return ln.lstrip("#").strip() if re.match(r"#{2,4}\s", ln) else None

def check_group(g):
    mism=[]; nchk=0
    lines=open(os.path.join(REPO,g,"README.md")).read().splitlines()
    incr=load_incr(g); brows=load_baselines(g); crows=load_cont(g)
    # HAS_ST = a real single-thread SWEEP exists (baselines.csv is written only from per-treatment
    # bench.csv); NOT bool(incr) — G3's incremental.csv is its contention data, not a sweep.
    HAS_ST=bool(brows); HAS_CONT=bool(crows)
    def cn(nm,T,col):            # contention.csv value for (name,T) — first matching row (gate/base order-indep)
        for r in crows:
            if r["name"]==nm and r["threads"]==str(T):
                try: return float(r[col])
                except: return None
        return None
    def cn_kind(nm,kind,T,col):  # contention.csv value for (name,kind,T)
        for r in crows:
            if r["name"]==nm and r["threads"]==str(T) and (r["kind"]==kind or g=="3_contention"):
                try: return float(r[col])
                except: return None
        return None
    Ts=sorted({int(r["threads"]) for r in crows}) if crows else []
    part=None; sec=None; cur_t=None; cur_cond=None; rmode=None
    def M(s): mism.append(f"{g} {s}")
    for ln in lines:
        h=htext(ln)
        if h is not None:
            if h.startswith("Part A"): part="A"
            elif h.startswith("Part B"): part="B"
            mt=re.match(r"`([^`]+)`$",h)
            if mt: cur_t=mt.group(1); continue           # treatment sub-heading; keep current section
            sec=h; cur_cond=None
            if h=="Result":
                rmode = "cont" if (part=="B" or (HAS_CONT and not HAS_ST)) else "st"
            continue
        mc=re.match(r"\*\*"+COND_RE+r"\*\*",ln)
        if mc: cur_cond=mc.group(1); continue
        if not ln.startswith("| "): continue
        c=split_cells(ln)

        # ---- Result — single-thread (8 cols): placement|N|base cyc|base ns|var cyc|var ns|Δcyc|Δns
        if sec=="Result" and rmode=="st" and len(c)==8 and c[0] in ST_PLACE and cur_t and cur_cond:
            place=c[0]
            try: N=int(c[1])
            except: continue
            r_base=num(c[2]); r_var=num(c[4]); r_dcyc=num(c[6]); r_dns=num(c[7])
            csv_dcyc=incr_lookup(incr,cur_t,place,cur_cond,N,"incr_cyc_iter")
            csv_dns =incr_lookup(incr,cur_t,place,cur_cond,N,"incr_ns_iter")
            pool=base_pool(brows,cur_cond,N,"base_cyc_iter"); med=stats.median(pool) if pool else None
            nchk+=4
            if not close(r_dcyc,csv_dcyc,0.06): M(f"ST Δcyc {cur_t}/{place}/{cur_cond}/N{N}: README {r_dcyc} vs CSV {csv_dcyc}")
            if not close(r_dns,csv_dns,0.06):  M(f"ST Δns  {cur_t}/{place}/{cur_cond}/N{N}: README {r_dns} vs CSV {csv_dns}")
            if not close(r_base,med,0.06):     M(f"ST base {cur_t}/{place}/{cur_cond}/N{N}: README {r_base} vs median {med}")
            if med is not None and csv_dcyc is not None and not close(r_var,round(med+csv_dcyc,1),0.06):
                M(f"ST var {cur_t}/{place}/{cur_cond}/N{N}: README {r_var} vs round(base+Δ) {round(med+csv_dcyc,1)}")
            continue
        # ---- Result — contention (11 cols): kind|T|base cyc/op|treat|Δcyc|base ns|treat ns|Δns|l1|remote|gate
        if sec=="Result" and rmode=="cont" and len(c)==11 and re.match(r"^\d+$",c[1]) and cur_t:
            kind,T=c[0],int(c[1])
            for label,rv,col,tol in [("base",num(c[2]),"base_cyc_op",0.02),("treat",num(c[3]),"treat_cyc_op",0.02),
                                     ("Δcyc",num(c[4]),"incr_cyc_op",0.02),("basens",num(c[5]),"base_ns_op",0.01),
                                     ("treatns",num(c[6]),"treat_ns_op",0.01),("Δns",num(c[7]),"incr_ns_op",0.01),
                                     ("l1",num(c[8]),"treat_l1_op",0.001),("remote",num(c[9]),"treat_remote_op",0.001)]:
                cv=cn_kind(cur_t,kind,T,col); nchk+=1
                if not close(rv,cv,tol): M(f"CONT {label} {cur_t}/{kind}/T{T}: README {rv} vs CSV {cv}")
            continue
        # ---- Baseline cost (no fence / relaxed) [single-thread]: condition|N|n|ref cyc|min-max|σ|margin|...
        if sec and sec.startswith("Baseline cost") and "paired" not in sec and re.match(r"^"+COND_RE+r"$",c[0]) and len(c)>=7:
            cond=c[0]
            try: N=int(c[1])
            except: continue
            pool=base_pool(brows,cond,N,"base_cyc_iter")
            if not pool: continue
            ref=stats.median(pool); lo,hi=min(pool),max(pool); sd=stats.pstdev(pool) if len(pool)>1 else 0.0
            mg=max(hi-ref,ref-lo); mm=re.findall(r"[-+]?\d+\.?\d*",c[4])
            nchk+=4
            if num(c[2]) is not None and int(num(c[2]))!=len(pool): M(f"BASE n {cond}/N{N}: README {int(num(c[2]))} vs {len(pool)}")
            if not close(num(c[3]),ref,0.06): M(f"BASE ref {cond}/N{N}: README {num(c[3])} vs {ref:.1f}")
            if len(mm)>=2 and (not close(float(mm[0]),lo,0.06) or not close(float(mm[1]),hi,0.06)): M(f"BASE min-max {cond}/N{N}: README {c[4]} vs {lo:.1f}-{hi:.1f}")
            if not close(num(c[6]),mg,0.06): M(f"BASE margin {cond}/N{N}: README {c[6]} vs {mg:.1f}")
            continue
        # ---- Baseline cost (paired no-ordering phase) [contention]: `name`|T|n|ref|min-max|...
        if sec=="Baseline cost (paired no-ordering phase)" and c[0].startswith("`") and len(c)>=7 and re.match(r"^\d+$",c[1]):
            nm=c[0].strip("`"); T=c[1]
            nchk+=2
            if not close(num(c[3]),cn(nm,T,"base_cyc_op"),0.02): M(f"PBASE ref {nm}/T{T}: README {num(c[3])} vs CSV {cn(nm,T,'base_cyc_op')}")
            mm=re.findall(r"[-+]?\d+\.?\d*",c[4])
            if len(mm)>=2 and (not close(float(mm[0]),cn(nm,T,'base_cyc_op_min'),0.02) or not close(float(mm[1]),cn(nm,T,'base_cyc_op_max'),0.02)):
                M(f"PBASE min-max {nm}/T{T}: README {c[4]} vs {cn(nm,T,'base_cyc_op_min')}-{cn(nm,T,'base_cyc_op_max')}")
            continue
        # ---- Contention validation: `nm`|T|pin|overlap|mux|l1|remote|stall|verdict
        if sec=="Contention validation" and len(c)==9 and re.match(r"^\d+$",c[1]):
            nm=c[0].strip("`"); T=c[1]
            for label,rv,col,tol in [("mux",num(c[4]),"mux",0.002),("l1",num(c[5]),"treat_l1_op",0.002),("remote",num(c[6]),"treat_remote_op",0.002)]:
                cv=cn(nm,T,col); nchk+=1
                if not close(rv,cv,tol): M(f"CVAL {label} {nm}/T{T}: README {rv} vs CSV {cv}")
            continue
        # ---- Summary (Group 1 residency): | `t` | cond | Δ cyc/iter | after_group(N=1→64) | after_every(N=1→64)
        if sec=="Summary" and len(c)==5 and c[2] in ("Δ cyc/iter","Δ ns/iter"):
            if c[0].startswith("`"): cur_t=c[0].strip("`")
            if re.match(r"^"+COND_RE+r"$",c[1]): cur_cond=c[1]
            col = "incr_cyc_iter" if c[2]=="Δ cyc/iter" else "incr_ns_iter"
            for cell,pl in zip((c[3],c[4]),("after_group","after_every")):
                ends=re.findall(r"[-+]\d+\.?\d*",cell)
                if len(ends)<2: continue
                for N,rv in ((1,float(ends[0])),(64,float(ends[-1]))):
                    cv=incr_lookup(incr,cur_t,pl,cur_cond,N,col); nchk+=1
                    if not close(rv,cv,0.06): M(f"SUMR {col} {cur_t}/{pl}/{cur_cond}/N{N}: README {rv} vs CSV {cv}")
            continue
        # ---- Summary (fence groups G1/G2): | `t` | Δ cyc/iter | a → b | ... (4 combos, N=1→64)
        if sec=="Summary" and len(c)==6 and c[1] in ("Δ cyc/iter","Δ ns/iter"):
            if c[0].startswith("`"): cur_t=c[0].strip("`")
            col = "incr_cyc_iter" if c[1]=="Δ cyc/iter" else "incr_ns_iter"
            combos=[("after_group","miss"),("after_every","miss"),("after_group","hit"),("after_every","hit")]
            for cell,(pl,cond) in zip(c[2:6],combos):
                ends=re.findall(r"[-+]\d+\.?\d*",cell)
                if len(ends)<2: continue
                for N,rv in ((1,float(ends[0])),(64,float(ends[-1]))):
                    cv=incr_lookup(incr,cur_t,pl,cond,N,col); nchk+=1
                    if not close(rv,cv,0.06): M(f"SUM {col} {cur_t}/{pl}/{cond}/N{N}: README {rv} vs CSV {cv}")
            continue
        # ---- Summary (G4-A atomics): | `op` | cond | Δ cyc/iter | <4 order cells a → b> (N=1→64)
        if sec=="Summary" and len(c)==7 and c[2] in ("Δ cyc/iter","Δ ns/iter"):
            if c[0].startswith("`"): cur_t=c[0].strip("`")
            if c[1] in ("miss","hit"): cur_cond=c[1]
            col = "incr_cyc_iter" if c[2]=="Δ cyc/iter" else "incr_ns_iter"
            for cell,order in zip(c[3:7],("acquire","release","acqrel","seqcst")):
                ends=re.findall(r"[-+]\d+\.?\d*",cell)
                if len(ends)<2: continue
                for N,rv in ((1,float(ends[0])),(64,float(ends[-1]))):
                    cv=incr_lookup(incr,cur_t,order,cur_cond,N,col); nchk+=1
                    if not close(rv,cv,0.06): M(f"SUMA {col} {cur_t}/{order}/{cur_cond}/N{N}: README {rv} vs CSV {cv}")
            continue
        # ---- Summary (G3 / G4-B contention): unit in c[1] = Δ cyc/op|Δ ns/op
        if sec=="Summary" and len(c)>=6 and c[1] in ("Δ cyc/op","Δ ns/op"):
            col = "incr_cyc_op" if c[1]=="Δ cyc/op" else "incr_ns_op"
            tol = 0.02 if col=="incr_cyc_op" else 0.005
            mt=re.findall(r"`(\w+)`",c[0])
            if "→" in c[2]:        # G4-B: | `op` | unit | a → b per order (T0→T1)
                if mt: cur_t=mt[0]
                for cell,order in zip(c[2:6],("acquire","release","acqrel","seqcst")):
                    ends=re.findall(r"[-+]\d+\.?\d*",cell)
                    if len(ends)<2: continue
                    for T,rv in ((Ts[0],float(ends[0])),(Ts[-1],float(ends[-1]))):
                        cv=cn_kind(cur_t,order,T,col); nchk+=1
                        if not close(rv,cv,tol): M(f"SUMB {col} {cur_t}/{order}/T{T}: README {rv} vs CSV {cv}")
            else:                  # G3: | `ldar` (RCsc) | unit | v per T  (or the gap row, 2 backticks)
                if len(mt)==1: cur_t=mt[0]
                is_gap = len(mt)==2 or "gap" in c[0]
                if not mt and not is_gap and c[0].strip(): cur_t=None
                for T,cell in zip(Ts,c[2:2+len(Ts)]):
                    rv=num(cell)
                    if rv is None: continue
                    if is_gap or (not c[0].strip() and cur_t=="GAP"):
                        cv_a=cn("ldar",T,col); cv_p=cn("ldapr",T,col)
                        cv=(cv_a-cv_p) if (cv_a is not None and cv_p is not None) else None
                        nchk+=1
                        if not close(rv,cv,tol*2): M(f"SUMG gap {col}/T{T}: README {rv} vs CSV {cv}")
                        cur_t="GAP"
                    elif cur_t and cur_t!="GAP":
                        cv=cn(cur_t,T,col); nchk+=1
                        if not close(rv,cv,tol): M(f"SUMG {col} {cur_t}/T{T}: README {rv} vs CSV {cv}")
            continue
        # ---- At a glance: fence 4-col "cyc (ns)" / atomic 4-col worst / contention 5-col "a → b cyc (ns)" + gap row
        if sec and (sec=="At a glance" or sec.startswith("A · ") or sec.startswith("B · ")) and ln.startswith("| "):
            mt=re.findall(r"`(\w+)`",c[0])
            nm=mt[0] if mt else None
            is_gap = "gap" in c[0]
            if len(c)==5 and crows and "→" in c[1+ (0 if is_gap else 0)]:
                # contention glance: | nm | base a → b cyc (ns) | Δ a → b cyc (ns) | trend | gate  (gap row: base="—")
                def ends4(cell):   # [cyc0, cyc1, ns0, ns1]
                    v=re.findall(r"[-+]?\d+\.?\d*",cell.replace("**",""))
                    return [float(x) for x in v[:4]] if len(v)>=4 else None
                def rowsel(name,T,absmax):
                    rs=[r for r in crows if r["name"]==name and r["threads"]==str(T)]
                    if not rs: return None
                    if absmax: return max(rs,key=lambda r:abs(float(r["incr_cyc_op"])))
                    return sorted(rs,key=lambda r:float(r["base_cyc_op"]))[len(rs)//2]
                if is_gap:
                    for cell,(cc,nc) in [(c[1],("base_cyc_op","base_ns_op")),(c[2],("treat_cyc_op","treat_ns_op"))]:
                        v=ends4(cell)
                        if not v: continue
                        for i,(T,col) in enumerate([(Ts[0],cc),(Ts[-1],cc),(Ts[0],nc),(Ts[-1],nc)]):
                            ra=rowsel("ldar",T,True); rp=rowsel("ldapr",T,True)
                            cv=float(ra[col])-float(rp[col]) if (ra and rp) else None
                            nchk+=1
                            if not close(v[i],cv,0.06): M(f"GLANCE gap {col}/T{T}: README {v[i]} vs CSV {cv}")
                elif nm:
                    combos = [(c[1],True,"base_cyc_op","base_ns_op"),(c[2],True,"treat_cyc_op","treat_ns_op")]
                    for cell,absmax,cc,nc in combos:
                        v=ends4(cell)
                        if not v: continue
                        for i,(T,col) in enumerate([(Ts[0],cc),(Ts[-1],cc),(Ts[0],nc),(Ts[-1],nc)]):
                            r=rowsel(nm,T,absmax)
                            cv=float(r[col]) if r else None
                            nchk+=1
                            if not close(v[i],cv,0.06): M(f"GLANCE {col} {nm}/T{T}: README {v[i]} vs CSV {cv}")
            elif len(c)==6 and incr and "cyc (" in c[1]:
                # Group-1 residency glance: | inst | l1 | l2 | l3 | dram | gate (after_every·N=64, per-iter).
                # ABSOLUTE values: baseline row (no backtick) = pooled base; treatment rows = base + Δ.
                for cell,cond in zip(c[1:5],("l1","l2","l3","dram")):
                    v=re.findall(r"[-+]?\d+\.?\d*",cell)
                    if len(v)<2: continue
                    pc=base_pool(brows,cond,64,"base_cyc_iter"); pn=base_pool(brows,cond,64,"base_ns_iter")
                    mc=stats.median(pc) if pc else None; mn=stats.median(pn) if pn else None
                    nchk+=2
                    if nm:   # treatment row: absolute = base + Δ
                        dc=incr_lookup(incr,nm,"after_every",cond,64,"incr_cyc_iter")
                        dn=incr_lookup(incr,nm,"after_every",cond,64,"incr_ns_iter")
                        ec=round(mc+dc,1) if (mc is not None and dc is not None) else None
                        en=round(mn+dn,1) if (mn is not None and dn is not None) else None
                        if not close(float(v[0]),ec,0.06): M(f"GLANCE abs cyc {nm}/{cond}: README {v[0]} vs base+Δ {ec}")
                        if not close(float(v[1]),en,0.06): M(f"GLANCE abs ns {nm}/{cond}: README {v[1]} vs base+Δ {en}")
                    else:    # baseline row: pooled base median
                        if not close(float(v[0]),round(mc,1) if mc is not None else None,0.06): M(f"GLANCE base cyc {cond}: README {v[0]} vs {mc}")
                        if not close(float(v[1]),round(mn,1) if mn is not None else None,0.06): M(f"GLANCE base ns {cond}: README {v[1]} vs {mn}")
            elif len(c)==4 and incr and nm and "cyc (" in c[1]:
                # fence glance: | t | Δ miss "x cyc (y ns)" | Δ hit | gate  — per-ITERATION (Result values)
                for cell,cond in [(c[1],"miss"),(c[2],"hit")]:
                    v=re.findall(r"[-+]?\d+\.?\d*",cell)
                    if len(v)<2: continue
                    dc=incr_lookup(incr,nm,"after_every",cond,64,"incr_cyc_iter")
                    dn=incr_lookup(incr,nm,"after_every",cond,64,"incr_ns_iter")
                    nchk+=2
                    if not close(float(v[0]),dc,0.06): M(f"GLANCE Δcyc {nm}/{cond}: README {v[0]} vs CSV {dc}")
                    if not close(float(v[1]),dn,0.06): M(f"GLANCE Δns {nm}/{cond}: README {v[1]} vs CSV {dn}")
            elif len(c)==5 and incr and nm and "N=64" not in c[1] and "cyc (" in c[1] and "→" not in c[1]:
                # atomic glance: | op | base miss "x cyc (y ns)" | base hit | worst Δ "a cyc (b ns) / c cyc (d ns)" | gate
                for cell,cond in [(c[1],"miss"),(c[2],"hit")]:
                    v=re.findall(r"[-+]?\d+\.?\d*",cell)
                    if len(v)<2: continue
                    pc=base_pool(brows,cond,64,"base_cyc_iter"); pn=base_pool(brows,cond,64,"base_ns_iter")
                    nchk+=2
                    if not close(float(v[0]),stats.median(pc) if pc else None,0.06): M(f"GLANCE base cyc {nm}/{cond}: {v[0]}")
                    if not close(float(v[1]),stats.median(pn) if pn else None,0.06): M(f"GLANCE base ns {nm}/{cond}: {v[1]}")
                halves=c[3].split("/")
                for half,cond in zip(halves,("miss","hit")):
                    v=re.findall(r"[-+]\d+\.?\d*",half)
                    if len(v)<2: continue
                    pairs=[(incr_lookup(incr,nm,o,cond,64,"incr_cyc_iter"),incr_lookup(incr,nm,o,cond,64,"incr_ns_iter")) for o in ("acquire","release","acqrel","seqcst")]
                    pairs=[p for p in pairs if p[0] is not None]
                    wc,wn=max(pairs,key=lambda p:abs(p[0])) if pairs else (None,None)
                    nchk+=2
                    if not close(float(v[0]),wc,0.06): M(f"GLANCE worst cyc {nm}/{cond}: {v[0]} vs {wc}")
                    if not close(float(v[1]),wn,0.06): M(f"GLANCE worst ns {nm}/{cond}: {v[1]} vs {wn}")
            continue
    return nchk,mism

def check_g5(g):
    """G5 (5_release_serialization) has a non-standard layout (one CSV row per (variation,N/x);
    baseline=str / treatment=stlr, paired). Dedicated handler: every base/treat/Δ cell in the
    GROUP README (At a glance + Result, Var1 & Var2) AND in the TOP README §6.5 must match
    out/release_serial.csv cell-for-cell. Also checks the group README's Baseline cost table
    (ref/margin cyc+ns) and Pattern/cache validation table (l1/ll/mux). Variation context = the
    `Variation 1`/`Variation 2` marker; base/Δ rows keyed by leading int, aux tables by (var=c0,key=c1)."""
    mism=[]; nchk=[0]
    rows=load_csv(os.path.join(REPO,g,"out","release_serial.csv"))
    if not rows: return 0,[f"{g}: no out/release_serial.csv"]
    def find(var,key):
        kcol = "N" if var==1 else "hits_x"
        for r in rows:
            if int(r["variation"])==var and int(r[kcol])==key: return r
        return None
    def cellnums(cell):   # [cyc, ns] from "x.y cyc (a.b ns)" (strip **, ≈0, − sign)
        v=re.findall(r"[-+]?\d+\.?\d*", cell.replace("**","").replace("−","-"))
        return [float(x) for x in v]
    def scan(tag, lines):
        var=None
        for ln in lines:
            if "Variation 1" in ln: var=1
            elif "Variation 2" in ln: var=2
            if not ln.startswith("| "): continue
            c=split_cells(ln)
            # Baseline cost (11 col) & Validation (9 col): leading cell = variation, c[1] = key (N/x)
            if len(c) in (9,11) and re.match(r"^\d+$",c[0]) and re.match(r"^\d+$",c[1]):
                rr=find(int(c[0]),int(c[1]))
                if not rr: mism.append(f"{tag} aux var{c[0]} key{c[1]}: no CSV row"); continue
                pairs = ([(c[3],"base_cyc_iter"),(c[6],"base_cyc_margin"),(c[7],"base_ns_iter"),(c[10],"base_ns_margin")]
                         if len(c)==11 else  # validation (9 col): l1 (base/treat), ll (base/treat), mux
                         [(c[3],"base_l1_iter"),(c[4],"treat_l1_iter"),(c[5],"base_ll_iter"),(c[6],"treat_ll_iter"),(c[7],"mux")])
                for cell,col in pairs:
                    nums=cellnums(cell)
                    if not nums: continue
                    nchk[0]+=1
                    if abs(nums[0]-float(rr[col]))>0.06:
                        mism.append(f"{tag} aux {col} var{c[0]} key{c[1]}: README {nums[0]} vs CSV {rr[col]}")
                continue
            if not re.match(r"^\d+$", c[0]) or var is None: continue
            if   len(c)==5:               bcell,tcell,dcell=c[1],c[2],c[3]   # At-a-glance / Result Var1
            elif len(c)==6 and var==2:    bcell,tcell,dcell=c[2],c[3],c[4]   # Result Var2 (has miss/iter col)
            else: continue
            r=find(var,int(c[0]))
            if not r: mism.append(f"{tag} var{var} key{c[0]}: no CSV row"); continue
            for label,cell,(cc,nc) in [("base",bcell,("base_cyc_iter","base_ns_iter")),
                                       ("treat",tcell,("treat_cyc_iter","treat_ns_iter")),
                                       ("Δ",dcell,("d_cyc_iter","d_ns_iter"))]:
                nums=cellnums(cell)
                if len(nums)<2: continue
                for got,col in ((nums[0],cc),(nums[1],nc)):
                    want=float(r[col]); nchk[0]+=1
                    if abs(got-want)>0.06:
                        mism.append(f"{tag} var{var} {label} {col} key{c[0]}: README {got} vs CSV {want}")
    scan("G5", open(os.path.join(REPO,g,"README.md")).read().splitlines())          # group README
    scan("top§6.5", open(os.path.join(REPO,"README.md")).read().splitlines())        # top README §6.5
    return nchk[0],mism

total=0; allm=[]
for g in GROUPS:
    n,mm = check_g5(g) if g=="5_release_serialization" else check_group(g)
    total+=n; allm+=mm
    print(f"[{g}] checked {n} cells, {len(mm)} mismatch")
    for x in mm[:40]: print("   ✗", x)
print(f"\nTOTAL: {total} cells checked, {len(allm)} mismatches")
sys.exit(1 if allm else 0)
