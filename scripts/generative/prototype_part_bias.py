#!/usr/bin/env python3
"""PROTOTYPE (#414, throwaway): Part bias's curve, linear vs curved, by ear and eye.

Runs the harness's part_bias_prototype (tests/spec/generator/prototype/part_bias_main.cpp) for a few genres and writes
one folder: CSVs, WAVs, a self-contained index.html with the plots and players, and a README. Build the harness first
(`./dbt test --generator --no-run`, then `./dbt exec "cmake --build build/tests --config Debug --target
part_bias_prototype"`).

    python3 scripts/generative/prototype_part_bias.py [--out DIR] [--no-audio] [--jobs N]
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import html
import itertools
import json
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BINARY = os.path.join(
    REPO, "build", "tests", "spec", "generator", "Debug", "part_bias_prototype"
)
DEFAULT_OUT = os.path.expanduser(
    "~/.cache/deluge-generative-m3/prototype-414-part-bias"
)

# (genre, biased part, why that part)
GENRES = [
    ("techno-detroit", "MOTIF", "a pitched part with its own Density gate (27)"),
    ("psytrance", "LEAD", "a pitched part with its own Density gate (30)"),
    (
        "house",
        "DRUMS",
        "a Kit part: its lanes follow it (Ride, Shaker and Conga gated at 35)",
    ),
]
MACROS = ["density", "energy", "complexity", "motion"]
MODELS = ["linear", "curved", "gateshift"]
MODEL_LABEL = {
    "linear": "1 linear (v+o, clamped)",
    "curved": "3 curved (headroom)",
    "gateshift": "3 curved + 4 gate shift",
}
BIASES = [-20, -10, -5, 5, 10, 20]
AUDIO_MACROS = ["density", "energy"]
MIX_BIASES = [-20, -10, 10, 20]
SWEEP_BARS = 32
ARC_BARS = 80
ARC_CENTRES = [25, 40]
# Density while another macro sweeps: above every biased part's gate, so the part plays throughout.
HELD_DENSITY = 40
STEPS_PER_BAR = 16
FULL = 3200  # macro units at 50


def models_for(macro: str) -> list[str]:
    # The gate shift differs from curved only at the Density gates.
    return MODELS if macro == "density" else MODELS[:2]


def name(genre, macro, model, bias, kind, centre=None):
    tag = f"c{centre}-" if centre is not None else ""
    return (
        f"{genre}-{macro}-{tag}{model}-{bias:+d}-{kind}"
        if bias
        else f"{genre}-{macro}-{tag}ref-{kind}"
    )


def job(out, genre, part, macro, model, bias, solo, audio, centre=None):
    kind = "solo" if solo else "mix"
    stem = name(genre, macro, model, bias, kind, centre)
    args = [
        BINARY,
        "--genre",
        genre,
        "--part",
        part,
        "--macro",
        macro,
        "--model",
        model,
        "--bias",
        str(bias),
    ]
    args += ["--bars", str(ARC_BARS if centre is not None else SWEEP_BARS)]
    if centre is not None:
        args += ["--centre", str(centre)]
    if macro != "density":
        args += ["--density", str(HELD_DENSITY)]
    if solo:
        args.append("--solo")
    if not audio:
        args.append("--no-audio")
    args += ["--out", os.path.join(out, "renders", stem)]
    return stem, args


def run(args):
    proc = subprocess.run(args, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"{' '.join(args)}: {proc.stderr}")
    return proc.stderr


def read_csv(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    return rows


def per_bar(rows, part_names, biased_part):
    bars = {}
    for r in rows:
        b = int(r["bar"])
        bar = bars.setdefault(
            b,
            {
                "section": r["section"],
                "shared": int(r["shared"]),
                "biased": int(r["biased"]),
                "notes": {},
            },
        )
        for p in part_names:
            bar["notes"][p] = bar["notes"].get(p, 0) + int(r[p])
    out = [bars[b] for b in sorted(bars)]
    steps = [(int(r["shared"]), int(r["biased"])) for r in rows]
    return {"bars": out, "steps": steps, "part": biased_part}


def max_jump(steps):
    """The largest step-to-step change in the biased value beyond the shared value's own, in points."""
    worst = 0.0
    for (s0, b0), (s1, b1) in itertools.pairwise(steps):
        worst = max(worst, abs(b1 - b0) / 64 - abs(s1 - s0) / 64)
    return worst


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--no-audio", action="store_true")
    ap.add_argument("--jobs", type=int, default=4)
    a = ap.parse_args()
    if not os.path.exists(BINARY):
        print(
            f"no {BINARY}: build part_bias_prototype first (see --help)",
            file=sys.stderr,
        )
        return 2
    os.makedirs(os.path.join(a.out, "renders"), exist_ok=True)

    jobs = []
    for genre, part, _ in GENRES:
        for macro in MACROS:
            audio_macro = macro in AUDIO_MACROS and not a.no_audio
            jobs.append(job(a.out, genre, part, macro, "linear", 0, True, audio_macro))
            if audio_macro:
                jobs.append(job(a.out, genre, part, macro, "linear", 0, False, True))
            for model in models_for(macro):
                for bias in BIASES:
                    jobs.append(
                        job(a.out, genre, part, macro, model, bias, True, audio_macro)
                    )
                    if audio_macro and bias in MIX_BIASES:
                        jobs.append(
                            job(a.out, genre, part, macro, model, bias, False, True)
                        )
        for macro in AUDIO_MACROS:
            for centre in ARC_CENTRES:
                jobs.append(
                    job(a.out, genre, part, macro, "linear", 0, True, False, centre)
                )
                for model in ("linear", "curved"):
                    for bias in (-20, 20):
                        jobs.append(
                            job(
                                a.out,
                                genre,
                                part,
                                macro,
                                model,
                                bias,
                                True,
                                False,
                                centre,
                            )
                        )
    print(f"{len(jobs)} renders into {a.out}", file=sys.stderr)
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        for warning in pool.map(lambda j: run(j[1]), jobs):
            if warning.strip():
                print(warning.strip(), file=sys.stderr)

    data = {
        "genres": [],
        "biases": BIASES,
        "models": MODEL_LABEL,
        "arcCentres": ARC_CENTRES,
    }
    jumps = []
    for genre, part, why in GENRES:
        g = {"genre": genre, "part": part, "why": why, "macros": {}}
        for macro in MACROS:
            ref_rows = read_csv(
                os.path.join(
                    a.out, "renders", name(genre, macro, "", 0, "solo") + ".csv"
                )
            )
            part_names = [
                k
                for k in ref_rows[0]
                if k not in ("step", "bar", "section", "shared", "biased")
            ]
            m = {
                "parts": part_names,
                "ref": per_bar(ref_rows, part_names, part),
                "runs": {},
                "arc": {},
            }
            for model in models_for(macro):
                for bias in BIASES:
                    rows = read_csv(
                        os.path.join(
                            a.out,
                            "renders",
                            name(genre, macro, model, bias, "solo") + ".csv",
                        )
                    )
                    m["runs"][f"{model}{bias:+d}"] = per_bar(rows, part_names, part)
            if macro in AUDIO_MACROS:
                for centre in ARC_CENTRES:
                    arc = {}
                    stem = name(genre, macro, "", 0, "solo", centre)
                    arc["ref"] = per_bar(
                        read_csv(os.path.join(a.out, "renders", stem + ".csv")),
                        part_names,
                        part,
                    )
                    for model in ("linear", "curved"):
                        for bias in (-20, 20):
                            stem = name(genre, macro, model, bias, "solo", centre)
                            res = per_bar(
                                read_csv(os.path.join(a.out, "renders", stem + ".csv")),
                                part_names,
                                part,
                            )
                            arc[f"{model}{bias:+d}"] = res
                            jumps.append(
                                (
                                    genre,
                                    macro,
                                    centre,
                                    model,
                                    bias,
                                    max_jump(res["steps"]),
                                )
                            )
                    for v in arc.values():
                        del v["steps"]
                    m["arc"][str(centre)] = arc
            for v in [m["ref"], *m["runs"].values()]:
                del v["steps"]
            m["audio"] = macro in AUDIO_MACROS and not a.no_audio
            g["macros"][macro] = m
        data["genres"].append(g)
    data["jumps"] = [
        {
            "genre": j[0],
            "macro": j[1],
            "centre": j[2],
            "model": j[3],
            "bias": j[4],
            "points": round(j[5], 2),
        }
        for j in jumps
    ]
    data["mixBiases"] = MIX_BIASES
    with open(os.path.join(a.out, "index.html"), "w") as f:
        f.write(PAGE.replace("/*DATA*/null", json.dumps(data, separators=(",", ":"))))
    with open(os.path.join(a.out, "README.md"), "w") as f:
        f.write(
            README.format(out=html.escape(a.out), bars=SWEEP_BARS, arc_bars=ARC_BARS)
        )
    worst = max(jumps, key=lambda j: j[5]) if jumps else None
    print(os.path.join(a.out, "index.html"))
    if worst:
        print(
            f"largest Build/Arc jump beyond the shared value's own: {worst[5]:.2f} points ({worst[:5]})"
        )
    return 0


README = """# Part bias curve prototype (#414)

PROTOTYPE, throwaway. It answers one question for #303: should a part's Part bias move its macro **linearly**
(model 1: `v + o`, clamped 0–50) or **curved** (model 3, headroom-proportional: `o > 0`: `v + o/50·(50−v)`,
`o < 0`: `v·(1 + o/50)`)? Model 4 (a threshold shift) is shown for Density's gates as "curved + gate shift".

## How to play it

Open `index.html` in a browser (double-click; it plays from `file://`, no server). Everything is on that page:

- **The curves** at the top: what each model does to a part's value for each bias, against the shared value.
- **One section per genre and macro.** The Recipe value sweeps 0 → 50 over {bars} bars (Groove, no Intro). One part
  is biased, by −20, −10, −5, +5, +10 and +20 points. Pick a bias with the buttons; the plots show it:
  - **Effective value**: the shared value after the Arc (grey) and the biased part's, per model.
  - **Notes per bar** of the biased part, per model, against the unbiased part (grey).
  - **Who plays**: a row per part for each bar it plays a note in; the biased part's rows per model at the bottom.
- Energy, Complexity and Motion sweep with Density held at 40, above every biased part's gate, so it plays.
- **Listen** (Density and Energy): a player per render. *Solo* is the biased part alone; *Mix* is the whole piece.
  Each player has **A/B**: it switches between the biased render and the unbiased reference at the same position, so
  you hear the bias and nothing else. The bias buttons pick the renders.
- **The Arc pass**: the value held at 25 and at 40 while the Arc goes Groove → Break → Build → Drop over {arc_bars} bars, for
  ±20. Here the bias is applied after the Arc (`effectiveOn`), so neither model jumps when the Build starts; the
  table under it gives the largest jump measured. What differs is the Build's rise: linear clamps at 50, so a +20
  part stops rising early.

## Files

- `index.html`: the page.
- `renders/*.wav`: the renders (the harness's rough render, our stand-in voices plus pack/ samples).
  `<genre>-<macro>-<model>-<bias>-solo|mix.wav`, `…-ref-…` is the unbiased reference.
- `renders/*.csv`: a line per step: section, the shared value and the biased value (macro units, 64 a point), notes
  per part.

## What to answer on #303

Linear or curved? The same curve for all four macros, or which differ? And for Density's gates: the curve, or the
gate shift (model 4)?

Regenerate with `python3 scripts/generative/prototype_part_bias.py` (see its `--help`).
"""

PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>Part bias curve prototype (#414)</title>
<style>
:root{color-scheme:light dark;--fg:#222;--bg:#fff;--mute:#888;--grid:#ddd;--lin:#d9480f;--cur:#1971c2;--gs:#2b8a3e;--ref:#999}
@media (prefers-color-scheme:dark){:root{--fg:#ddd;--bg:#161616;--grid:#333;--lin:#ff8a4c;--cur:#5aa9ff;--gs:#69db7c;--ref:#777}}
body{font:14px/1.45 system-ui,sans-serif;color:var(--fg);background:var(--bg);margin:0 auto;max-width:1200px;padding:16px}
h1{font-size:22px}h2{margin-top:40px;border-top:1px solid var(--grid);padding-top:16px}h3{margin:18px 0 6px}
.note{color:var(--mute)}.row{display:flex;gap:16px;flex-wrap:wrap}.panel{flex:1;min-width:340px}
svg{width:100%;height:auto;display:block}svg text{fill:var(--fg);font-size:10px}
.ax{stroke:var(--grid)}.lin{stroke:var(--lin)}.cur{stroke:var(--cur)}.gs{stroke:var(--gs)}.ref{stroke:var(--ref)}
button{font:inherit;padding:3px 9px;margin:2px;border:1px solid var(--grid);background:none;color:var(--fg);border-radius:4px;cursor:pointer}
button.on{background:var(--cur);color:#fff;border-color:var(--cur)}
.players{display:grid;grid-template-columns:repeat(auto-fill,minmax(330px,1fr));gap:8px}
.player{border:1px solid var(--grid);border-radius:6px;padding:6px 8px}.player audio{width:100%}
.legend span{margin-right:14px}.sw{display:inline-block;width:18px;height:3px;vertical-align:middle;margin-right:4px}
table{border-collapse:collapse}td,th{padding:2px 8px;border-bottom:1px solid var(--grid);text-align:right}
</style></head><body>
<h1>Part bias: linear or curved? <span class="note">(#414 prototype, throwaway)</span></h1>
<p>One part per genre gets a Part bias on one macro while the Recipe value sweeps 0 → 50. Compare
<b style="color:var(--lin)">model 1, linear</b> (<code>v+o</code>, clamped) with
<b style="color:var(--cur)">model 3, curved</b> (headroom-proportional), and for Density
<b style="color:var(--gs)">curved notes + model 4, a gate shift</b>. The bias is applied after the Arc
(<code>effectiveOn</code>). Renders are the harness's rough render, not the Deluge's sound. See README.md.</p>
<div id="curves"></div>
<div id="main"></div>
<script>
const D=/*DATA*/null;
const COL={linear:'lin',curved:'cur',gateshift:'gs'};
const FULL=3200;
function el(t,a={},...k){const e=document.createElement(t);for(const[n,v]of Object.entries(a)){if(n==='on')e.onclick=v;else e.setAttribute(n,v)}for(const c of k)e.append(c);return e}
function svg(w,h,body){return `<svg viewBox="0 0 ${w} ${h}" xmlns="http://www.w3.org/2000/svg">${body}</svg>`}
function line(pts,cls,dash){return `<polyline fill="none" class="${cls}" stroke-width="1.8" ${dash?'stroke-dasharray="5 3"':''} points="${pts.map(p=>p.join(',')).join(' ')}"/>`}
function frame(w,h,l,b,xmax,ymax,xlab,ylab,yticks){
  let s='';for(const t of yticks){const y=h-b-(t/ymax)*(h-b-8);s+=`<line class="ax" x1="${l}" x2="${w-4}" y1="${y}" y2="${y}"/><text x="${l-4}" y="${y+3}" text-anchor="end">${t}</text>`}
  for(let x=0;x<=xmax;x+=(xmax<=12?1:xmax<=40?4:10)){const X=l+(x/xmax)*(w-l-4);s+=`<text x="${X}" y="${h-b+12}" text-anchor="middle">${x}</text>`}
  s+=`<text x="${(w+l)/2}" y="${h-2}" text-anchor="middle">${xlab}</text><text x="10" y="${(h-b)/2}" transform="rotate(-90 10 ${(h-b)/2})" text-anchor="middle">${ylab}</text>`;return s}
function pts(vals,w,h,l,b,xmax,ymax){return vals.map((v,i)=>[l+(i/xmax)*(w-l-4),h-b-(v/ymax)*(h-b-8)])}
function sections(bars,w,h,l,b){let s='',start=0,k=0;for(let i=1;i<=bars.length;i++){if(i===bars.length||bars[i].section!==bars[start].section){const x0=l+(start/(bars.length-1))*(w-l-4),x1=l+(Math.min(i,bars.length-1)/(bars.length-1))*(w-l-4);s+=`<text x="${x0+2}" y="${10+10*(k++%2)}" class="note" fill-opacity=".6">${bars[start].section}</text><line class="ax" x1="${x0}" x2="${x0}" y1="0" y2="${h-b}"/>`;start=i}}return s}
// The curves.
(function(){
  const W=380,H=260,L=34,B=28;const c=document.getElementById('curves');c.append(el('h2',{},'The curves'));
  const row=el('div',{class:'row'});
  for(const model of ['linear','curved']){let s=frame(W,H,L,B,50,50,'shared value (points)','part value',[0,10,20,30,40,50]);
    for(const o of [-20,-10,-5,0,5,10,20]){const v=[];for(let x=0;x<=50;x++){let y;if(model==='linear')y=Math.min(50,Math.max(0,x+o));else y=o>=0?x+o/50*(50-x):x*(1+o/50);v.push(y)}
      s+=line(pts(v,W,H,L,B,50,50),o===0?'ref':COL[model],o<0);}
    const p=el('div',{class:'panel'});p.innerHTML=`<b>${D.models[model]}</b> (bias −20 … +20)`+svg(W,H,s);row.append(p)}
  const p=el('div',{class:'panel note'});p.innerHTML=`<p><b>Read:</b> linear keeps the offset exact but hits 0 or 50 early: at a shared 45, +10 gives +5, and a −20 part is silent until the shared value passes 20. Curved never saturates early: +10 is 10/50 of the way to 50, so it shrinks near the top, and −10 shrinks the value by a fifth, so it shrinks near 0. Both ends stay reachable.</p><p>Gate shift (Density only): the part's Density gate sees <code>shared + bias</code>, unclamped, so a gate at 30 biased +10 opens at a shared 20, exactly as linear; notes and thinning follow the curve.</p>`;row.append(p);c.append(row)})();
// Per genre and macro.
const main=document.getElementById('main');
for(const g of D.genres){
  main.append(el('h2',{},`${g.genre}: ${g.part} biased`));main.append(el('p',{class:'note'},`${g.part}: ${g.why}.`));
  for(const[macro,m]of Object.entries(g.macros)){
    const sec=el('section');main.append(sec);sec.append(el('h3',{},`${macro[0].toUpperCase()+macro.slice(1)} swept 0 → 50`));
    const models=Object.keys(D.models).filter(k=>m.runs[`${k}+5`]);
    let bias=10;const btns=el('div',{});const plots=el('div',{class:'row'});const players=el('div',{class:'players'});
    for(const b of D.biases){const bt=el('button',{on:()=>{bias=b;draw()}},(b>0?'+':'')+b);bt.dataset.b=b;btns.append(bt)}
    sec.append(el('span',{class:'note'},'Bias: '),btns,plots);
    if(m.audio){sec.append(el('h3',{},'Listen'),el('p',{class:'note'},'A/B switches to the unbiased reference at the same position. Solo: the biased part alone. Mix: the whole piece.'),players)}
    function draw(){
      for(const bt of btns.children)bt.classList.toggle('on',+bt.dataset.b===bias);
      plots.innerHTML='';const W=380,H=200,L=34,B=28,n=m.ref.bars.length-1;
      // effective value
      let s=frame(W,H,L,B,n,50,'bar','value (points)',[0,10,20,30,40,50]);
      s+=sections(m.ref.bars,W,H,L,B);
      s+=line(pts(m.ref.bars.map(x=>x.shared/64),W,H,L,B,n,50),'ref',false);
      for(const k of models){const r=m.runs[`${k}${bias>0?'+':''}${bias}`];s+=line(pts(r.bars.map(x=>x.biased/64),W,H,L,B,n,50),COL[k],k==='gateshift')}
      plots.append(Object.assign(el('div',{class:'panel'}),{innerHTML:`<b>Effective value</b> <span class="note">grey: shared; colour: ${g.part} at the bias${macro==='density'?' (gate shift: its notes follow the curve; only its gate moves linearly)':''}</span>`+svg(W,H,s)}));
      // notes per bar
      const all=[m.ref,...models.map(k=>m.runs[`${k}${bias>0?'+':''}${bias}`])].map(r=>r.bars.map(x=>x.notes[g.part]));
      const ymax=Math.max(4,...all.flat());const step=Math.ceil(ymax/5);
      s=frame(W,H,L,B,n,ymax,'bar',`${g.part} notes per bar`,[0,1,2,3,4,5].map(i=>i*step).filter(t=>t<=ymax));
      s+=line(pts(all[0],W,H,L,B,n,ymax),'ref',false);
      models.forEach((k,i)=>{s+=line(pts(all[i+1],W,H,L,B,n,ymax),COL[k],k==='gateshift')});
      plots.append(Object.assign(el('div',{class:'panel'}),{innerHTML:`<b>Notes per bar</b> <span class="note">grey: unbiased</span>`+svg(W,H,s)}));
      // who plays
      const rows=[...m.parts.map(p=>({label:p+(p===g.part?' (unbiased)':''),bars:m.ref.bars.map(x=>x.notes[p]>0),cls:'ref'})),
        ...models.map(k=>({label:`${g.part} ${k}`,bars:m.runs[`${k}${bias>0?'+':''}${bias}`].bars.map(x=>x.notes[g.part]>0),cls:COL[k]}))];
      const RH=12,LW=110,WW=780;s='';const cw=(WW-LW-4)/(n+1);
      rows.forEach((r,i)=>{s+=`<text x="${LW-4}" y="${i*RH+11}" text-anchor="end">${r.label}</text>`;r.bars.forEach((on,b)=>{if(on)s+=`<rect x="${LW+b*cw}" y="${i*RH+2}" width="${cw-1}" height="${RH-4}" class="${r.cls}" style="fill:var(--${r.cls==='ref'?'ref':r.cls})"/>`})});
      plots.append(Object.assign(el('div',{class:'panel'}),{innerHTML:`<b>Who plays</b> <span class="note">a block per bar with a note</span>`+svg(WW,rows.length*RH+4,s).replace('<svg ','<svg style="max-width:780px" ')}));
      if(m.audio)drawPlayers();
    }
    function drawPlayers(){
      players.innerHTML='';const tag=(bias>0?'+':'')+bias;
      for(const kind of ['solo','mix']){
        if(kind==='mix'&&!D.mixBiases.includes(bias)){players.append(el('div',{class:'player note'},`Mix: rendered at ${D.mixBiases.join(', ')} only.`));continue}
        for(const k of models){
          const biased=`renders/${g.genre}-${macro}-${k}-${tag}-${kind}.wav`,ref=`renders/${g.genre}-${macro}-ref-${kind}.wav`;
          const au=el('audio',{controls:'',preload:'none',src:biased});let onRef=false;
          const ab=el('button',{on:()=>{const t=au.currentTime,playing=!au.paused;onRef=!onRef;au.src=onRef?ref:biased;au.currentTime=t;if(playing)au.play();ab.textContent=onRef?'B: reference (unbiased)':'A: biased';ab.classList.toggle('on',onRef)}},'A: biased');
          const lab=el('div',{},el('b',{style:`color:var(--${COL[k]})`},D.models[k]),` ${kind}, ${g.part} ${tag} `);
          players.append(el('div',{class:'player'},lab,au,ab,el('a',{href:biased,class:'note',style:'margin-left:8px'},'wav')))}
      }
    }
    draw();
    // Arc pass
    if(Object.keys(m.arc).length){
      sec.append(el('h3',{},'Arc pass: the value held while the Arc goes Groove → Break → Build → Drop'));
      const row=el('div',{class:'row'});sec.append(row);
      for(const[c,arc]of Object.entries(m.arc)){
        const W=380,H=200,L=34,B=28,n=arc.ref.bars.length-1;let s=frame(W,H,L,B,n,50,'bar','value (points)',[0,10,20,30,40,50]);
        s+=sections(arc.ref.bars,W,H,L,B);s+=line(pts(arc.ref.bars.map(x=>x.shared/64),W,H,L,B,n,50),'ref',false);
        for(const k of ['linear','curved'])for(const b of [-20,20]){const r=arc[`${k}${b>0?'+':''}${b}`];s+=line(pts(r.bars.map(x=>x.biased/64),W,H,L,B,n,50),COL[k],b<0)}
        row.append(Object.assign(el('div',{class:'panel'}),{innerHTML:`<b>Centre ${c}</b> <span class="note">solid +20, dashed −20; grey: shared</span>`+svg(W,H,s)}));
      }
    }
  }
}
main.append(el('h2',{},'Build jump check'));
const worst=D.jumps.reduce((a,j)=>j.points>a.points?j:a,{points:0});
main.append(el('p',{},`Largest step-to-step change in a biased part's value beyond the shared value's own change, over every Arc pass: ${worst.points} points`+(worst.genre?` (${worst.genre}, ${worst.macro}, centre ${worst.centre}, ${worst.model} ${worst.bias}).`:'.')+' Applied after effectiveOn, a part follows the shared Arc, so a Build start moves it no more than it moves the shared value (a curved bias moves it less, a clamped linear one not at all).'));
</script></body></html>
"""

if __name__ == "__main__":
    sys.exit(main())
