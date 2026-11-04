// PROTOTYPE — throwaway, never merge. Branch: prototype/scale-mapping.
// Wayfinder ticket: "Mapping steps onto 5- and 6-note scales" (gismya/deluge-generative#29).
//
// Renders one fixed pattern (bass, arp, triad pad, kick) through three rules for
// mapping the Generator's 7-step pitch space onto a 5- or 6-note scale:
//   modulo — scale[s mod n] + 12*floor(s/n)
//   rank   — round(d*n/7) within the octave (d = s mod 7)
//   snap   — resolve in a 7-note parent scale, snap to the nearest scale note (ties go down)
// Each file is 16 bars: C minor / small scale / C minor / small scale, 4 bars each,
// the same step material in every 4-bar block, so only the mapping changes.
//
// Run:  node prototypes/scale-mapping/render.mjs
// Then open prototypes/scale-mapping/out/index.html (WAV players, MIDI links, note tables).

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const OUT = path.join(path.dirname(fileURLToPath(import.meta.url)), "out");
fs.mkdirSync(OUT, { recursive: true });

// ---------------------------------------------------------------- scales
// Intervals copied from src/deluge/model/scale/preset_scales.h
const MINOR = [0, 2, 3, 5, 7, 8, 10]; // natural minor (Aeolian)
const LYDIAN = [0, 2, 4, 6, 7, 9, 11];
const SMALL = [
  { id: "whole-tone", name: "Whole Tone", notes: [0, 2, 4, 6, 8, 10], parent: LYDIAN, parentName: "Lydian" },
  { id: "blues", name: "Blues", notes: [0, 3, 5, 6, 7, 10], parent: MINOR, parentName: "Minor" },
  { id: "pentatonic-minor", name: "Pentatonic Minor", notes: [0, 3, 5, 7, 10], parent: MINOR, parentName: "Minor" },
  { id: "hirajoshi", name: "Hirajoshi", notes: [0, 2, 3, 7, 8], parent: MINOR, parentName: "Minor" },
];

const mod = (a, n) => ((a % n) + n) % n;

// Each mapping: (step, scale) -> semitones above the part's tonic.
const MAPPINGS = {
  modulo: (s, sc) => sc.notes[mod(s, sc.notes.length)] + 12 * Math.floor(s / sc.notes.length),
  rank: (s, sc) => {
    const n = sc.notes.length;
    const idx = Math.round((mod(s, 7) * n) / 7); // never .5 for n = 5, 6
    return sc.notes[idx % n] + 12 * (Math.floor(s / 7) + Math.floor(idx / n));
  },
  snap: (s, sc) => {
    const p = sc.parent[mod(s, 7)] + 12 * Math.floor(s / 7);
    let best = null;
    for (let oct = -1; oct <= 1; oct++)
      for (const iv of sc.notes) {
        const cand = 12 * (Math.floor(p / 12) + oct) + iv;
        const d = Math.abs(cand - p);
        if (best === null || d < best.d || (d === best.d && cand < best.c)) best = { d, c: cand }; // tie: down
      }
    return best.c;
  },
};
const sevenNote = (s) => MINOR[mod(s, 7)] + 12 * Math.floor(s / 7);

// ---------------------------------------------------------------- pattern (in steps)
// Register per role (see Pitch model, #21): tonic = C nearest the anchor; notes outside the span fold by octaves.
const REG = {
  bass: { tonic: 36, lo: 33, hi: 50 },
  pad: { tonic: 60, lo: 55, hi: 74 },
  arp: { tonic: 72, lo: 67, hi: 91 },
};
const fold = (m, r) => {
  while (m > r.hi) m -= 12;
  while (m < r.lo) m += 12;
  return m;
};

const BPM = 112;
const ROOTS = [0, 5, 2, 6]; // i – VI – III – VII in minor: C Ab Eb Bb
const CHORD_NAMES = ["i", "VI", "III", "VII"];

// One 4-bar block as {role, pos (16th in block), len (16ths), step, vel}
function block() {
  const ev = [];
  ROOTS.forEach((r, bar) => {
    const at = bar * 16;
    // bass: root, root, fifth, root, third, passing tone down to the next root area
    for (const [p, ds, len] of [[0, 0, 3], [3, 0, 2], [6, 4, 2], [8, 0, 3], [11, 2, 2], [14, -1, 2]])
      ev.push({ role: "bass", pos: at + p, len, step: r + ds, vel: p === 0 ? 110 : 90 });
    // pad: stacked-thirds triad, whole bar
    for (const ds of [0, 2, 4]) ev.push({ role: "pad", pos: at, len: 16, step: r + ds, vel: 70 });
    // arp: up/down over 1-3-5-7-8 in 16ths
    const shape = [0, 2, 4, 6, 7, 6, 4, 2];
    for (let i = 0; i < 16; i++) ev.push({ role: "arp", pos: at + i, len: 1, step: r + shape[i % 8], vel: i % 4 === 0 ? 100 : 75 });
    for (let q = 0; q < 4; q++) ev.push({ role: "kick", pos: at + q * 4, len: 2, step: 0, vel: 100 });
  });
  return ev;
}

// Resolve the 16-bar arrangement: blocks alternate C minor / small scale.
function arrange(sc, mapName) {
  const out = [];
  const markers = [];
  for (let b = 0; b < 4; b++) {
    const small = sc && b % 2 === 1;
    markers.push({ pos: b * 64, text: small ? `${sc.name} (${mapName})` : "C minor" });
    for (const e of block()) {
      let midi = 36;
      if (e.role !== "kick") {
        const semis = small ? MAPPINGS[mapName](e.step, sc) : sevenNote(e.step);
        midi = fold(REG[e.role].tonic + semis, REG[e.role]);
      }
      out.push({ ...e, pos: e.pos + b * 64, midi });
    }
  }
  return { events: out, markers };
}

// ---------------------------------------------------------------- audio
const SR = 44100;
const SIXTEENTH = 60 / BPM / 4;
const hz = (m) => 440 * Math.pow(2, (m - 69) / 12);

function render(events) {
  const total = Math.ceil((64 * 4 * SIXTEENTH + 1.5) * SR);
  const buf = new Float32Array(total);
  for (const e of events) {
    const start = Math.round(e.pos * SIXTEENTH * SR);
    const v = e.vel / 127;
    const f = hz(e.midi);
    if (e.role === "kick") {
      const n = Math.round(0.3 * SR);
      let ph = 0;
      for (let i = 0; i < n && start + i < total; i++) {
        const t = i / SR;
        ph += (2 * Math.PI * (45 + 90 * Math.exp(-t * 30))) / SR;
        buf[start + i] += 0.5 * v * Math.sin(ph) * Math.exp(-t * 12);
      }
    } else if (e.role === "bass") {
      const gate = e.len * SIXTEENTH * 0.9, n = Math.round((gate + 0.05) * SR);
      let ph = 0, lp = 0;
      for (let i = 0; i < n && start + i < total; i++) {
        const t = i / SR;
        ph = (ph + f / SR) % 1;
        const cut = 200 + 1800 * Math.exp(-t * 18);
        const a = 1 - Math.exp((-2 * Math.PI * cut) / SR);
        lp += a * (2 * ph - 1 - lp);
        const env = Math.min(1, t * 400) * (t < gate ? 1 : Math.exp(-(t - gate) * 60));
        buf[start + i] += 0.35 * v * lp * env;
      }
    } else if (e.role === "arp") {
      const n = Math.round(0.25 * SR);
      let ph = 0, lp = 0;
      const a = 1 - Math.exp((-2 * Math.PI * 2500) / SR);
      for (let i = 0; i < n && start + i < total; i++) {
        const t = i / SR;
        ph = (ph + f / SR) % 1;
        lp += a * ((ph < 0.3 ? 1 : -1) - lp);
        buf[start + i] += 0.09 * v * lp * Math.min(1, t * 500) * Math.exp(-t * 16);
      }
    } else if (e.role === "pad") {
      const gate = e.len * SIXTEENTH, n = Math.round((gate + 0.4) * SR);
      const det = [0.996, 1, 1.004];
      const ph = [0, 0.33, 0.66];
      let lp = 0;
      const a = 1 - Math.exp((-2 * Math.PI * 1400) / SR);
      for (let i = 0; i < n && start + i < total; i++) {
        const t = i / SR;
        let s = 0;
        for (let k = 0; k < 3; k++) { ph[k] = (ph[k] + (f * det[k]) / SR) % 1; s += 2 * ph[k] - 1; }
        lp += a * (s / 3 - lp);
        const env = Math.min(1, t / 0.12) * (t < gate ? 1 : Math.exp(-(t - gate) * 10));
        buf[start + i] += 0.12 * v * lp * env;
      }
    }
  }
  let peak = 0;
  for (const x of buf) peak = Math.max(peak, Math.abs(x));
  const g = 0.89 / peak;
  const pcm = Buffer.alloc(44 + total * 2);
  pcm.write("RIFF", 0); pcm.writeUInt32LE(36 + total * 2, 4); pcm.write("WAVE", 8);
  pcm.write("fmt ", 12); pcm.writeUInt32LE(16, 16); pcm.writeUInt16LE(1, 20); pcm.writeUInt16LE(1, 22);
  pcm.writeUInt32LE(SR, 24); pcm.writeUInt32LE(SR * 2, 28); pcm.writeUInt16LE(2, 32); pcm.writeUInt16LE(16, 34);
  pcm.write("data", 36); pcm.writeUInt32LE(total * 2, 40);
  for (let i = 0; i < total; i++) pcm.writeInt16LE(Math.round(Math.max(-1, Math.min(1, buf[i] * g)) * 32767), 44 + i * 2);
  return pcm;
}

// ---------------------------------------------------------------- MIDI (type 1, 480 PPQN)
function vlq(n) {
  const b = [n & 0x7f];
  while ((n >>= 7)) b.unshift((n & 0x7f) | 0x80);
  return b;
}
function track(evs) { // evs: [{tick, bytes}]
  evs.sort((a, b) => a.tick - b.tick || a.order - b.order);
  const data = [];
  let last = 0;
  for (const e of evs) { data.push(...vlq(e.tick - last), ...e.bytes); last = e.tick; }
  data.push(0, 0xff, 0x2f, 0);
  const h = Buffer.alloc(8); h.write("MTrk", 0); h.writeUInt32BE(data.length, 4);
  return Buffer.concat([h, Buffer.from(data)]);
}
const text = (type, s) => [0xff, type, ...vlq(Buffer.byteLength(s)), ...Buffer.from(s)];
function midiFile({ events, markers }) {
  const T = 120; // ticks per 16th
  const us = Math.round(60e6 / BPM);
  const conductor = [{ tick: 0, order: 0, bytes: [0xff, 0x51, 3, (us >> 16) & 255, (us >> 8) & 255, us & 255] },
    { tick: 0, order: 0, bytes: [0xff, 0x58, 4, 4, 2, 24, 8] },
    ...markers.map((m) => ({ tick: m.pos * T, order: 1, bytes: text(0x06, m.text) }))];
  const roles = [["bass", 0], ["arp", 1], ["pad", 2], ["kick", 9]];
  const tracks = [track(conductor)];
  for (const [role, ch] of roles) {
    const evs = [{ tick: 0, order: 0, bytes: text(0x03, role) }];
    for (const e of events.filter((x) => x.role === role)) {
      evs.push({ tick: e.pos * T, order: 2, bytes: [0x90 | ch, e.midi, e.vel] });
      evs.push({ tick: (e.pos + e.len) * T - 1, order: 1, bytes: [0x80 | ch, e.midi, 0] });
    }
    tracks.push(track(evs));
  }
  const h = Buffer.alloc(14);
  h.write("MThd", 0); h.writeUInt32BE(6, 4); h.writeUInt16BE(1, 8); h.writeUInt16BE(tracks.length, 10); h.writeUInt16BE(480, 12);
  return Buffer.concat([h, ...tracks]);
}

// ---------------------------------------------------------------- write files + index.html
const NAMES = ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"];
const nn = (semis) => NAMES[mod(semis, 12)] + (Math.floor(semis / 12) ? `<sup>${Math.floor(semis / 12) > 0 ? "+" : ""}${Math.floor(semis / 12)}</sup>` : "");
const pcName = (m) => NAMES[mod(m, 12)];

const files = [];
function emit(base, arr) {
  fs.writeFileSync(path.join(OUT, base + ".wav"), render(arr.events));
  fs.writeFileSync(path.join(OUT, base + ".mid"), midiFile(arr));
  files.push(base);
  console.log("wrote", base + ".wav/.mid");
}
emit("reference-c-minor", arrange(null, null));

const blockSecs = 16 * 4 * SIXTEENTH;
const t = (s) => `${Math.floor(s / 60)}:${String(Math.round(s % 60)).padStart(2, "0")}`;
let html = `<!doctype html><meta charset="utf-8"><title>PROTOTYPE: step → small-scale mapping</title>
<style>body{font:14px system-ui;margin:2em;max-width:1100px}table{border-collapse:collapse;margin:.5em 0 1em}td,th{border:1px solid #ccc;padding:3px 8px;text-align:center}
.dup{background:#fdd}.col{background:#ffe7b3}audio{width:260px;height:32px}h2{margin-top:2em;border-top:2px solid #333;padding-top:.5em}code{background:#eee;padding:0 3px}</style>
<h1>PROTOTYPE — mapping 7 steps onto 5- and 6-note scales</h1>
<p>Throwaway. Ticket: <a href="https://github.com/gismya/deluge-generative/issues/29">Mapping steps onto 5- and 6-note scales</a>.
Every file is 16 bars at ${BPM} BPM, key C: <b>C minor</b> (0:00–${t(blockSecs)}) → <b>small scale</b> (${t(blockSecs)}–${t(2 * blockSecs)}) → <b>C minor</b> → <b>small scale</b> (${t(3 * blockSecs)}–${t(4 * blockSecs)}).
Each 4-bar block plays the same steps: chords ${ROOTS.map((r, i) => `${CHORD_NAMES[i]} (step ${r})`).join(", ")}; bass root/root/5th/root/3rd/passing step −1; arp 1-3-5-7-8-7-5-3 in 16ths; pad stacked-thirds triad.
Registers fold out-of-span notes by octaves (bass ${REG.bass.lo}–${REG.bass.hi}, pad ${REG.pad.lo}–${REG.pad.hi}, arp ${REG.arp.lo}–${REG.arp.hi}). MIDI has bass ch1, arp ch2, pad ch3, kick ch10 and a marker at each scale change.</p>
<p>Reference (C minor throughout): <audio controls preload="none" src="reference-c-minor.wav"></audio> <a href="reference-c-minor.mid">MIDI</a></p>
<p><b>Rules.</b> <code>modulo</code>: <code>scale[s mod n] + 12·⌊s/n⌋</code>. <code>rank</code>: <code>round(d·n/7)</code> within the octave. <code>snap</code>: the step's note in a 7-note parent scale, snapped to the nearest scale note, ties down.
<span class="dup">Red</span> = a step lands on the same note as the step below it. <span class="col">Amber</span> = a triad collapses to fewer than three pitch classes.</p>`;

for (const sc of SMALL) {
  html += `<h2>${sc.name} <small>(${sc.notes.map(pcName).join(" ")}; snap parent: C ${sc.parentName})</small></h2><table><tr><th>mapping</th><th>listen</th><th>MIDI</th></tr>`;
  for (const m of Object.keys(MAPPINGS)) {
    const base = `${sc.id}-${m}`;
    emit(base, arrange(sc, m));
    html += `<tr><td><b>${m}</b></td><td><audio controls preload="none" src="${base}.wav"></audio></td><td><a href="${base}.mid">${base}.mid</a></td></tr>`;
  }
  html += `</table><table><tr><th>step</th>${[...Array(8).keys()].map((s) => `<th>${s}</th>`).join("")}</tr>`;
  html += `<tr><td>C minor</td>${[...Array(8).keys()].map((s) => `<td>${nn(sevenNote(s))}</td>`).join("")}</tr>`;
  for (const [m, fn] of Object.entries(MAPPINGS)) {
    html += `<tr><td>${m}</td>${[...Array(8).keys()].map((s) => `<td class="${s && fn(s, sc) === fn(s - 1, sc) ? "dup" : ""}">${nn(fn(s, sc))}</td>`).join("")}</tr>`;
  }
  html += `</table><table><tr><th>triad (steps)</th><th>C minor</th>${Object.keys(MAPPINGS).map((m) => `<th>${m}</th>`).join("")}</tr>`;
  ROOTS.forEach((r, i) => {
    const tri = [r, r + 2, r + 4];
    const cell = (fn) => {
      const ms = tri.map((s) => fold(REG.pad.tonic + fn(s), REG.pad));
      const cls = new Set(ms.map((x) => mod(x, 12))).size < 3 ? "col" : "";
      return `<td class="${cls}">${ms.map(pcName).join("–")}</td>`;
    };
    html += `<tr><td>${CHORD_NAMES[i]} (${tri.join(",")})</td>${cell(sevenNote)}${Object.values(MAPPINGS).map((fn) => cell((s) => fn(s, sc))).join("")}</tr>`;
  });
  html += `</table>`;
}
fs.writeFileSync(path.join(OUT, "index.html"), html);
console.log("open", path.join(OUT, "index.html"));
