#!/usr/bin/env python3
"""make_figures.py — publication figures for barrierbench_extend, plotted ONLY from the
locked CSVs (no hand-typed numbers). matplotlib is absent on this host, so we emit clean
hand-rolled grayscale SVG line charts (dash + marker + shade ⇒ B/W-distinguishable).

Reads:
  1_store_side/processed/1_store_side_incremental.csv     (schema A)
  2_load_side/processed/2_load_side_incremental.csv       (schema A)
  3_contention/_contention/out/contention.csv             (schema B)
  4_atomics/processed/4_atomics_incremental.csv           (schema A, placement = order)
  4_atomics/_contention/out/contention.csv                (schema B)
Writes:  figures/*.svg
"""
import csv, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # barrierbench_extend/
OUT  = os.path.join(ROOT, "figures")
os.makedirs(OUT, exist_ok=True)
FOOT = ("GH200 ARM Neoverse-V2, 3.375 GHz fixed (1 cyc ≈ 0.296 ns), 1,000,000 iters/repeat; "
        "paired baseline+treatment; PMU via perf_event_open (user-mode); objdump-verified.")

def rows(path):
    with open(os.path.join(ROOT, path)) as f: return list(csv.DictReader(f))
def fnum(r, k):
    try: return float(r[k])
    except Exception: return float("nan")

# ---- six grayscale line styles (stroke shade + dash + marker) for B/W distinction ----
STYLES = [
    dict(c="#000000", dash="",          mk="circle"),
    dict(c="#000000", dash="7,4",       mk="square"),
    dict(c="#5a5a5a", dash="2,3",       mk="triangle"),
    dict(c="#5a5a5a", dash="9,3,2,3",   mk="diamond"),
    dict(c="#8c8c8c", dash="",          mk="plus"),
    dict(c="#8c8c8c", dash="6,4",       mk="circleo"),
]
def _marker(mk, x, y, c):
    s = 3.4
    if mk == "circle":   return f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{s}" fill="{c}"/>'
    if mk == "circleo":  return f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{s}" fill="white" stroke="{c}" stroke-width="1.2"/>'
    if mk == "square":   return f'<rect x="{x-s:.1f}" y="{y-s:.1f}" width="{2*s}" height="{2*s}" fill="{c}"/>'
    if mk == "triangle": return f'<polygon points="{x:.1f},{y-s-0.5:.1f} {x-s:.1f},{y+s:.1f} {x+s:.1f},{y+s:.1f}" fill="{c}"/>'
    if mk == "diamond":  return f'<polygon points="{x:.1f},{y-s-0.5:.1f} {x-s-0.5:.1f},{y:.1f} {x:.1f},{y+s+0.5:.1f} {x+s+0.5:.1f},{y:.1f}" fill="{c}"/>'
    if mk == "plus":     return (f'<line x1="{x-s-0.5:.1f}" y1="{y:.1f}" x2="{x+s+0.5:.1f}" y2="{y:.1f}" stroke="{c}" stroke-width="1.6"/>'
                                 f'<line x1="{x:.1f}" y1="{y-s-0.5:.1f}" x2="{x:.1f}" y2="{y+s+0.5:.1f}" stroke="{c}" stroke-width="1.6"/>')
    return ""

def _nice(mx):
    if mx <= 0: return 1.0
    import math
    e = math.floor(math.log10(mx)); b = mx / 10**e
    nb = 1 if b<=1 else 2 if b<=2 else 5 if b<=5 else 10
    step = nb * 10**e
    return math.ceil(mx/ (step/ (4 if nb in (2,10) else 5)) ) * (step/(4 if nb in (2,10) else 5))

def _panel(ox, oy, PW, PH, p):
    """one line panel at (ox,oy). p: dict(xlabel,ylabel,xcats,series[,ymax]). returns svg list."""
    o=[]; xs=p["xcats"]; n=len(xs)
    allv=[v for s in p["series"] for v in s["y"] if v==v]
    ymax = p.get("ymax") or _nice(max(allv) if allv else 1)
    ymin = min(0.0, min(allv) if allv else 0.0)
    if ymin<0: ymin = _nice(-ymin)*-1
    x0,y0 = ox+54, oy+30; w,h = PW-70, PH-70; baseY=y0+h
    def X(i): return x0 + (w*(i/(n-1)) if n>1 else w/2)
    def Y(v): return baseY - (v-ymin)/(ymax-ymin)*h
    # gridlines + y ticks
    for t in range(6):
        gv=ymin+(ymax-ymin)*t/5; gy=Y(gv)
        o.append(f'<line x1="{x0}" y1="{gy:.1f}" x2="{x0+w}" y2="{gy:.1f}" stroke="#e6e6e6" stroke-width="1"/>')
        o.append(f'<text x="{x0-6}" y="{gy+3:.1f}" font-size="10" text-anchor="end" fill="#333">{("%g"%round(gv,2))}</text>')
    # zero line bold if ymin<0
    if ymin<0: o.append(f'<line x1="{x0}" y1="{Y(0):.1f}" x2="{x0+w}" y2="{Y(0):.1f}" stroke="#999" stroke-width="1"/>')
    o.append(f'<line x1="{x0}" y1="{y0}" x2="{x0}" y2="{baseY}" stroke="black" stroke-width="1.3"/>')
    o.append(f'<line x1="{x0}" y1="{baseY}" x2="{x0+w}" y2="{baseY}" stroke="black" stroke-width="1.3"/>')
    for i,xc in enumerate(xs):
        o.append(f'<text x="{X(i):.1f}" y="{baseY+15:.1f}" font-size="10.5" text-anchor="middle">{xc}</text>')
    o.append(f'<text x="{x0+w/2:.1f}" y="{baseY+30:.1f}" font-size="11.5" text-anchor="middle">{p["xlabel"]}</text>')
    o.append(f'<text x="{ox+13}" y="{y0+h/2:.1f}" font-size="11.5" text-anchor="middle" transform="rotate(-90 {ox+13} {y0+h/2:.1f})">{p["ylabel"]}</text>')
    # series
    for s in p["series"]:
        st=STYLES[s["style"]%len(STYLES)]; pts=[]
        for i,v in enumerate(s["y"]):
            if v!=v: continue
            pts.append((X(i),Y(v)))
        if len(pts)>=2:
            d=" ".join(("M" if k==0 else "L")+f"{x:.1f} {y:.1f}" for k,(x,y) in enumerate(pts))
            o.append(f'<path d="{d}" fill="none" stroke="{st["c"]}" stroke-width="1.7"'+(f' stroke-dasharray="{st["dash"]}"' if st["dash"] else "")+'/>')
        for (x,y) in pts: o.append(_marker(st["mk"],x,y,st["c"]))
    return o

def fig(path, title, subtitle, panels, foot=FOOT):
    PW,PH = 560,420; gap=24; n=len(panels)
    W = 30 + n*PW + (n-1)*gap + 20; H = 96 + PH + 30
    L=[f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family="sans-serif">',
       f'<rect width="{W}" height="{H}" fill="white"/>',
       f'<text x="{W/2:.0f}" y="24" text-anchor="middle" font-size="16" font-weight="bold">{title}</text>',
       f'<text x="{W/2:.0f}" y="42" text-anchor="middle" font-size="11.5" fill="#444">{subtitle}</text>']
    # shared legend (from first panel's series labels+styles)
    leg=panels[0]["series"]; lx=30
    for s in leg:
        st=STYLES[s["style"]%len(STYLES)]
        L.append(f'<line x1="{lx}" y1="60" x2="{lx+26}" y2="60" stroke="{st["c"]}" stroke-width="1.8"'+(f' stroke-dasharray="{st["dash"]}"' if st["dash"] else "")+'/>')
        L.append(_marker(st["mk"], lx+13, 60, st["c"]))
        L.append(f'<text x="{lx+31}" y="63.5" font-size="11">{s["label"]}</text>')
        lx += 34 + len(s["label"])*6.6 + 14
    for pi,p in enumerate(panels):
        L += _panel(30 + pi*(PW+gap), 78, PW, PH, p)
    L.append(f'<text x="{30}" y="{H-8}" font-size="9" fill="#666">{foot}</text>')
    L.append('</svg>')
    open(os.path.join(OUT,path),"w").write("\n".join(L))
    return path

NS=[1,2,4,8,16,32,64]; TS=[1,2,4,8]
def series_from(rws, treats, key, cond, place, ncol="stores", vcol="incr_cyc_op"):
    out=[]
    for i,t in enumerate(treats):
        m={int(r[ncol]):fnum(r,vcol) for r in rws if r["treatment"]==t and r["condition"]==cond and r["placement"]==place}
        out.append(dict(label=key.get(t,t), y=[m.get(n,float("nan")) for n in NS], style=i))
    return out

made=[]
# ---------- G1 store-side ----------
g1=rows("1_store_side/processed/1_store_side_incremental.csv")
G1T=["dmb_ish","dmb_sy","dmb_ishst","dmb_st","stlr"]
G1L={"dmb_ish":"dmb ish (full)","dmb_sy":"dmb sy (full)","dmb_ishst":"dmb ishst (store)","dmb_st":"dmb st (store)","stlr":"stlr (store-release)"}
made.append(fig("g1_store_side_miss.svg",
    "G1 store-side ordering cost vs N (cache-missing store stream)",
    "incremental cyc/store · condition = miss · placement = after_every · 10 repeats (median)",
    [dict(xlabel="N (stores before the ordering op)", ylabel="Δ cyc / store (treatment − baseline)",
          xcats=NS, series=series_from(g1,G1T,G1L,"miss","after_every"))]))
made.append(fig("g1_store_side_hit.svg",
    "G1 store-side ordering cost vs N — cache-resident floor (hit)",
    "incremental cyc/store · condition = hit · placement = after_every · 10 repeats (median)",
    [dict(xlabel="N (stores before the ordering op)", ylabel="Δ cyc / store (treatment − baseline)",
          xcats=NS, series=series_from(g1,G1T,G1L,"hit","after_every"))]))

# ---------- G2 load-side ----------
g2=rows("2_load_side/processed/2_load_side_incremental.csv")
G2T=["dmb_ish","dmb_sy","dmb_ishld","dmb_ld","ldar","ldapr"]
G2L={"dmb_ish":"dmb ish (full)","dmb_sy":"dmb sy (full)","dmb_ishld":"dmb ishld (load)","dmb_ld":"dmb ld (load)","ldar":"ldar (RCsc)","ldapr":"ldapr (RCpc)"}
made.append(fig("g2_load_side_miss.svg",
    "G2 load-side ordering cost vs N (random load stream)",
    "incremental cyc/load · condition = miss · placement = after_every · 10 repeats (median)",
    [dict(xlabel="N (loads before the ordering op)", ylabel="Δ cyc / load (treatment − baseline)",
          xcats=NS, series=series_from(g2,G2T,G2L,"miss","after_every"))]))
made.append(fig("g2_load_side_hit.svg",
    "G2 load-side ordering cost vs N — cache-resident floor (hit)",
    "incremental cyc/load · condition = hit · placement = after_every · 10 repeats (median)",
    [dict(xlabel="N (loads before the ordering op)", ylabel="Δ cyc / load (treatment − baseline)",
          xcats=NS, series=series_from(g2,G2T,G2L,"hit","after_every"))]))

# ---------- G3 contention (the money figure) ----------
g3=rows("3_contention/_contention/out/contention.csv")
def g3map(name,col):
    m={int(r["threads"]):fnum(r,col) for r in g3 if r["name"]==name}; return [m.get(t,float("nan")) for t in TS]
made.append(fig("g3_contention_gap.svg",
    "G3 load-acquire completion stall vs contention (single shared line)",
    "Δ cyc/op (treatment − baseline) · T threads on distinct cores · 15 repeats (median) · T=1 = uncontended ref",
    [dict(xlabel="T (threads contending on one line)", ylabel="Δ cyc / op (ordering cost)",
          xcats=TS, series=[dict(label="ldar (RCsc) — pays the stall", y=g3map("ldar","incr_cyc_op"), style=0),
                             dict(label="ldapr (RCpc) — skips it",      y=g3map("ldapr","incr_cyc_op"), style=2)])]))
# evidence twin: L1D_REFILL/op and backend-mem stall fraction vs T (ldar)
made.append(fig("g3_contention_evidence.svg",
    "G3 coherence evidence behind the stall (ldar, RCsc)",
    "per-op coherence + exposed stall vs T · 15 repeats (median) · single shared line",
    [dict(xlabel="T (threads)", ylabel="fraction", xcats=TS, ymax=1.0,
          series=[dict(label="L1D_REFILL / op (line bounces)", y=g3map("ldar","treat_l1_op"), style=0),
                  dict(label="backend-mem stall / cycles",      y=g3map("ldar","treat_stall_frac"), style=2)])]))

# ---------- G4 atomics ----------
g4c=rows("4_atomics/_contention/out/contention.csv")
OPS=["ldadd","swp","cas"]; ORDS=["acquire","release","acqrel","seqcst"]
def g4c_base(op):  # relaxed RMW cost (base) per T — any order's base row (relaxed phase)
    m={}
    for r in g4c:
        if r["name"]==op: m[int(r["threads"])]=fnum(r,"base_cyc_op")
    return [m.get(t,float("nan")) for t in TS]
def g4c_surch(op):  # mean ordering surcharge over the 4 orders per T
    per={t:[] for t in TS}
    for r in g4c:
        if r["name"]==op: per[int(r["threads"])].append(fnum(r,"incr_cyc_op"))
    return [ (sum(v)/len(v) if v else float("nan")) for t in TS for v in [per[t]] ]
made.append(fig("g4a_contended.svg",
    "G4 contended atomic: the cost is the RMW instruction, not the memory order",
    "single shared line · base = relaxed RMW · surcharge = ordered − relaxed (mean of 4 orders) · 15 repeats (median)",
    [dict(xlabel="T (threads)", ylabel="cyc / op — relaxed RMW (base)", xcats=TS,
          series=[dict(label="ldadd", y=g4c_base("ldadd"), style=0),
                  dict(label="swp",   y=g4c_base("swp"),   style=2),
                  dict(label="cas",   y=g4c_base("cas"),   style=1)]),
     dict(xlabel="T (threads)", ylabel="Δ cyc / op — ordering surcharge (≈ 0)", xcats=TS,
          series=[dict(label="ldadd", y=g4c_surch("ldadd"), style=0),
                  dict(label="swp",   y=g4c_surch("swp"),   style=2),
                  dict(label="cas",   y=g4c_surch("cas"),   style=1)])]))
# G4(b) uncontended order surcharge vs N (single-thread), per op small-multiples, miss
g4u=rows("4_atomics/processed/4_atomics_incremental.csv")
def g4u_panels(cond):
    pans=[]
    for op in OPS:
        ser=[]
        for i,o in enumerate(ORDS):
            m={int(r["stores"]):fnum(r,"incr_cyc_op") for r in g4u if r["treatment"]==op and r["placement"]==o and r["condition"]==cond}
            ser.append(dict(label=o, y=[m.get(n,float("nan")) for n in NS], style=i))
        pans.append(dict(xlabel=f"N — {op}", ylabel="Δ cyc / op (order − relaxed)", xcats=NS, series=ser))
    return pans
made.append(fig("g4b_uncontended_surcharge_miss.svg",
    "G4 uncontended atomic ordering surcharge vs N (single thread)",
    "Δ cyc/op = ordered − relaxed · condition = miss · 10 repeats (median) · ≈0 (small cells are run-to-run noise)",
    g4u_panels("miss")))

print("wrote", len(made), "figures to", OUT)
for m in made: print("  figures/"+m)
