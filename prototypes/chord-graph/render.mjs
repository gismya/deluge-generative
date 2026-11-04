// PROTOTYPE — throwaway, never merge. Branch: prototype/chord-graph.
// Wayfinder ticket: "Chord graph across a genre's scales" (gismya/deluge-generative#31).
//
// Question: can one degree-based chord graph per genre sound right in every 7-note scale
// the genre rolls, or does it need more? Which of the 12 seven-note presets stay in v1?
//
// A PERF-1-style weighted walk over scale degrees (ideas only, hand-written weights):
//   weight = 1 + tonic pull (home) + graph edge (+ colour edge), A-B-A allowed,
//   guard ×0.15, cadence target +20 on the phrase's last chord. The phrase's first chord is always
//   home (PERF-1 gives home +30 there but can't stay put; forced here so every phrase lands).
// Variants:
//   guard    — PERF-1 guard: ×0.15 on degrees whose triad has a tritone fifth; a guarded
//              cadence target swaps to V, else IV.
//   colour   — guard + a Generator-wide colour-move row for the scale (Dorian, Phrygian,
//              Lydian, Mixolydian, Harmonic Minor).
//   strict   — the guard widened to every triad that isn't plain major or minor (augmented,
//              major ♭5, non-tertian). Rendered only for scales where it differs from `guard`.
// Every variant draws the same random numbers per chord, so the same seed gives the same walk
// wherever the weights agree. Same seeds in every scale, too.
//
// Run:  node prototypes/chord-graph/render.mjs
// Then open prototypes/chord-graph/out/index.html (WAV players, MIDI links, chord tables).

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const OUT = path.join(path.dirname(fileURLToPath(import.meta.url)), "out");
fs.mkdirSync(OUT, { recursive: true });

// ---------------------------------------------------------------- scales
// Intervals copied from src/deluge/model/scale/preset_scales.h (the 12 seven-note presets).
const SCALES = [
  { id: "major", name: "Major", notes: [0, 2, 4, 5, 7, 9, 11] },
  { id: "minor", name: "Minor", notes: [0, 2, 3, 5, 7, 8, 10] },
  { id: "dorian", name: "Dorian", notes: [0, 2, 3, 5, 7, 9, 10] },
  { id: "phrygian", name: "Phrygian", notes: [0, 1, 3, 5, 7, 8, 10] },
  { id: "lydian", name: "Lydian", notes: [0, 2, 4, 6, 7, 9, 11] },
  { id: "mixolydian", name: "Mixolydian", notes: [0, 2, 4, 5, 7, 9, 10] },
  { id: "locrian", name: "Locrian", notes: [0, 1, 3, 5, 6, 8, 10] },
  { id: "melodic-minor", name: "Melodic Minor", notes: [0, 2, 3, 5, 7, 9, 11] },
  { id: "harmonic-minor", name: "Harmonic Minor", notes: [0, 2, 3, 5, 7, 8, 11] },
  { id: "hungarian-minor", name: "Hungarian Minor", notes: [0, 2, 3, 6, 7, 8, 11] },
  { id: "marva", name: "Marva", notes: [0, 1, 4, 6, 7, 9, 11] },
  { id: "arabian", name: "Arabian", notes: [0, 2, 4, 5, 6, 8, 10] },
];
const byId = Object.fromEntries(SCALES.map((s) => [s.id, s]));

// Generator-wide colour-move rows: [from degree, to degree, added weight]. Degrees are 0-based (0 = I).
const COLOUR = {
  dorian: { label: "i–IV", edges: [[0, 3, 5], [3, 0, 4]] },
  phrygian: { label: "i–♭II", edges: [[0, 1, 5], [1, 0, 5]] },
  lydian: { label: "I–II", edges: [[0, 1, 5], [1, 0, 4]] },
  mixolydian: { label: "I–♭VII", edges: [[0, 6, 5], [6, 0, 4]] },
  "harmonic-minor": { label: "V–i", edges: [[4, 0, 6], [3, 4, 3]] },
};

const mod = (a, n) => ((a % n) + n) % n;
const semi = (sc, step) => sc.notes[mod(step, 7)] + 12 * Math.floor(step / 7);
const NOTE = ["C", "D♭", "D", "E♭", "E", "F", "G♭", "G", "A♭", "A", "B♭", "B"];
const MAJOR = [0, 2, 4, 5, 7, 9, 11];
const ROMAN = ["I", "II", "III", "IV", "V", "VI", "VII"];

// ---------------------------------------------------------------- chord analysis
function triad(sc, d) {
  const r = semi(sc, d);
  return { third: semi(sc, d + 2) - r, fifth: semi(sc, d + 4) - r, seventh: semi(sc, d + 6) - r };
}
function quality(sc, d) {
  const { third, fifth } = triad(sc, d);
  if (third === 4 && fifth === 7) return "maj";
  if (third === 3 && fifth === 7) return "min";
  if (third === 3 && fifth === 6) return "dim";
  if (third === 4 && fifth === 8) return "aug";
  return "odd"; // major ♭5, sus-like, non-tertian
}
const guarded = (sc, d, strict) => {
  const q = quality(sc, d);
  return strict ? q !== "maj" && q !== "min" : triad(sc, d).fifth === 6;
};
function roman(sc, d) {
  const acc = { "-1": "♭", "1": "♯", "-2": "𝄫", "2": "𝄪" }[String(sc.notes[d] - MAJOR[d])] || "";
  const q = quality(sc, d);
  const { third, fifth } = triad(sc, d);
  let r = ROMAN[d];
  if (third < 4) r = r.toLowerCase();
  return acc + r + { maj: "", min: "", dim: "°", aug: "+", odd: `[${third},${fifth}]` }[q];
}
function chordName(sc, d, kind) {
  const root = NOTE[sc.notes[d]];
  const { third, fifth, seventh } = triad(sc, d);
  const q = quality(sc, d);
  if (kind === 3) return root + { maj: "", min: "m", dim: "°", aug: "+", odd: `[${third},${fifth}]` }[q];
  const k = `${q}:${seventh}`;
  const n = {
    "maj:11": "maj7", "maj:10": "7", "min:10": "m7", "min:11": "m(maj7)", "dim:10": "m7♭5", "dim:9": "°7",
    "aug:11": "+maj7", "aug:10": "+7",
  }[k] || `[${third},${fifth},${seventh}]`;
  return root + n + (kind === 9 ? "/9" : "");
}

// ---------------------------------------------------------------- RNG
function mulberry32(a) {
  return () => {
    a |= 0; a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
const stream = (seed, salt) => mulberry32(seed * 7919 + salt * 104729);

// ---------------------------------------------------------------- graphs (hand-written)
const HOUSE = {
  name: "House", bpm: 122, hr: 2, cadEvery: 8, cadTo: 4, tonic: 2, back: true,
  // i→VI, i→iv, i→VII, VI→VII, VII→i, iv→i, iv→VII, VI→iv, III→VI, ii→v, v→i
  edges: [[0, 5, 5], [0, 3, 4], [0, 6, 4], [5, 6, 4], [6, 0, 5], [3, 0, 3], [3, 6, 3], [5, 3, 3], [2, 5, 3], [1, 4, 3], [4, 0, 3]],
};
const AMBIENT = {
  name: "Ambient", bpm: 80, hr: 4, cadEvery: 4, cadTo: 3, tonic: 1, back: true,
  // I→IV, IV→I, I→vi, vi→IV, IV→V, V→I, iii→IV
  edges: [[0, 3, 4], [3, 0, 4], [0, 5, 3], [5, 3, 3], [3, 4, 2], [4, 0, 2], [2, 3, 2]],
};

function cadTarget(sc, to, strict) {
  if (!guarded(sc, to, strict)) return to;
  return !guarded(sc, 4, strict) ? 4 : 3;
}

function walk(graph, sc, variant, seed, n) {
  const rng = stream(seed, 1);
  const colour = variant === "colour" ? COLOUR[sc.id] : null;
  const strict = variant === "strict";
  const seq = [0];
  let prev = -1, cur = 0;
  for (let k = 1; k < n; k++) {
    const pos = k % graph.cadEvery;
    const r = rng();
    if (pos === 0) { prev = cur; cur = 0; seq.push(0); continue; }
    const w = Array(7).fill(0);
    for (let d = 0; d < 7; d++) {
      if (d === cur) continue;
      let x = 1 + (d === 0 ? graph.tonic : 0);
      for (const [a, b, v] of graph.edges) if (a === cur && b === d) x += v;
      if (colour) for (const [a, b, v] of colour.edges) if (a === cur && b === d) x += v;
      if (!graph.back && d === prev) x = 0;
      if (guarded(sc, d, strict)) x *= 0.15;
      if (pos === graph.cadEvery - 1 && d === cadTarget(sc, graph.cadTo, strict)) x += 20;
      if (pos === graph.cadEvery - 1 && d === 0) x = 0; // next phrase starts home anyway
      w[d] = x;
    }
    let acc = r * w.reduce((a, b) => a + b, 0), pick = 0;
    for (let d = 0; d < 7; d++) { acc -= w[d]; if (acc < 0 && w[d] > 0) { pick = d; break; } }
    prev = cur; cur = pick; seq.push(cur);
  }
  return seq;
}

// ---------------------------------------------------------------- voicing
function permutations(a) {
  if (a.length <= 1) return [a];
  return a.flatMap((x, i) => permutations([...a.slice(0, i), ...a.slice(i + 1)]).map((p) => [x, ...p]));
}
// Brute-force voice leading (same idea as PERF-1's): nearest octave per voice, cost = movement + 40 per unison.
function voiceLead(pcs, prev, lo, hi) {
  let best = null;
  for (const p of permutations(pcs)) {
    const v = p.map((pc, i) => {
      let m = 12 * Math.round((prev[i] - pc) / 12) + pc;
      while (m > hi) m -= 12;
      while (m < lo) m += 12;
      return m;
    });
    let cost = v.reduce((s, m, i) => s + Math.abs(m - prev[i]), 0);
    for (let i = 0; i < v.length; i++) for (let j = i + 1; j < v.length; j++) if (v[i] === v[j]) cost += 40;
    if (!best || cost < best.cost) best = { v, cost };
  }
  return best.v.sort((a, b) => a - b);
}
const STACK = { 3: [0, 2, 4, 0], 7: [0, 2, 4, 6], 9: [0, 2, 6, 8] }; // triads double the root
const pcsOf = (sc, d, kind) => STACK[kind].map((o) => mod(semi(sc, d + o), 12));
const fold = (m, lo, hi) => { while (m > hi) m -= 12; while (m < lo) m += 12; return m; };
const TONIC = 48; // key C

// ---------------------------------------------------------------- arrangements
// Events: {role, pos (16ths), len (16ths), midi, vel}. Markers: {pos, text}.
function houseArrangement(sc, seq, seed) {
  const kindRng = stream(seed, 2);
  const ev = [], markers = [], chords = [];
  let prev = [55, 60, 64, 67];
  const STAB = [[0, 2], [3, 1], [6, 2], [10, 2], [16 + 2, 2], [16 + 7, 1], [16 + 10, 2], [16 + 14, 2]];
  seq.forEach((d, i) => {
    const last = i === seq.length - 1;
    const at = i * 32;
    const kind = kindRng() < 0.55 ? 7 : 9;
    prev = voiceLead(pcsOf(sc, d, kind), prev, 55, 79);
    chords.push({ d, kind });
    markers.push({ pos: at, text: `${roman(sc, d)} ${chordName(sc, d, kind)}` });
    const bars = last ? 1 : 2;
    for (const [p, len] of STAB.filter(([p]) => p < bars * 16)) for (const m of prev) ev.push({ role: "stab", pos: at + p, len, midi: m, vel: p === 0 ? 100 : 85 });
    const root = fold(TONIC + semi(sc, d), 33, 45);
    for (let b = 0; b < bars; b++) {
      for (const p of [2, 6, 10, 14]) ev.push({ role: "bass", pos: at + b * 16 + p, len: 2, midi: root, vel: 100 });
      for (let q = 0; q < 4; q++) ev.push({ role: "kick", pos: at + b * 16 + q * 4, len: 1, midi: 36, vel: 110 });
      for (const p of [4, 12]) ev.push({ role: "clap", pos: at + b * 16 + p, len: 1, midi: 39, vel: 90 });
      for (const p of [2, 6, 10, 14]) ev.push({ role: "hat", pos: at + b * 16 + p, len: 1, midi: 42, vel: 80 });
    }
    if (last) ev.push({ role: "bass", pos: at, len: 4, midi: root, vel: 110 });
  });
  return { events: ev, markers, chords, bpm: HOUSE.bpm, bars: (seq.length - 1) * 2 + 1 };
}

function ambientArrangement(sc, seq, seed) {
  const bellRng = stream(seed, 3);
  const ev = [], markers = [], chords = [];
  let prev = [52, 59, 64, 67];
  const L = AMBIENT.hr * 16;
  seq.forEach((d, i) => {
    const at = i * L;
    const last = i === seq.length - 1;
    const len = last ? 32 : L;
    prev = voiceLead(pcsOf(sc, d, 7), prev, 52, 76);
    chords.push({ d, kind: 7 });
    markers.push({ pos: at, text: `${roman(sc, d)} ${chordName(sc, d, 7)}` });
    for (const m of prev) ev.push({ role: "pad", pos: at, len, midi: m, vel: 80 });
    ev.push({ role: "bass", pos: at, len, midi: fold(TONIC + semi(sc, d), 33, 45), vel: 90, sustain: true });
    // bells: chord tones from a 2-octave ladder, on 8ths with low probability
    const ladder = [];
    for (const o of [0, 2, 4, 6, 7, 9, 11]) ladder.push(fold(TONIC + 24 + semi(sc, d + o), 72, 96));
    ladder.sort((a, b) => a - b);
    for (let p = 0; p < len; p += 2) {
      if (bellRng() < 0.18) ev.push({ role: "bell", pos: at + p, len: 4, midi: ladder[Math.floor(bellRng() * ladder.length)], vel: 60 + Math.floor(bellRng() * 30) });
    }
  });
  return { events: ev, markers, chords, bpm: AMBIENT.bpm, bars: (seq.length - 1) * AMBIENT.hr + 2, delay: true };
}

// Scale tour: every degree, triad for 2 beats then 7th for 2 beats, then home again.
function tourArrangement(sc) {
  const ev = [], markers = [];
  let prev = [55, 60, 64, 67];
  [0, 1, 2, 3, 4, 5, 6, 0].forEach((d, i) => {
    const at = i * 16;
    markers.push({ pos: at, text: `${roman(sc, d)} ${chordName(sc, d, 3)} → ${chordName(sc, d, 7)}` });
    prev = voiceLead(pcsOf(sc, d, 3), prev, 55, 79);
    for (const m of prev) ev.push({ role: "pad", pos: at, len: 8, midi: m, vel: 85 });
    prev = voiceLead(pcsOf(sc, d, 7), prev, 55, 79);
    for (const m of prev) ev.push({ role: "pad", pos: at + 8, len: 8, midi: m, vel: 85 });
    ev.push({ role: "bass", pos: at, len: 16, midi: fold(TONIC + semi(sc, d), 33, 45), vel: 90, sustain: true });
  });
  return { events: ev, markers, bpm: 110, bars: 8, attack: 0.02 };
}

// ---------------------------------------------------------------- audio
const SR = 44100;
const hz = (m) => 440 * Math.pow(2, (m - 69) / 12);
const noise = mulberry32(12345);

function render(arr) {
  const SIX = 60 / arr.bpm / 4;
  const total = Math.ceil((arr.bars * 16 * SIX + 3) * SR);
  const buf = new Float32Array(total);
  const add = (i, x) => { if (i < total) buf[i] += x; };
  for (const e of arr.events) {
    const start = Math.round(e.pos * SIX * SR);
    const v = e.vel / 127, f = hz(e.midi), gate = e.len * SIX;
    if (e.role === "kick") {
      let ph = 0;
      for (let i = 0; i < 0.3 * SR; i++) {
        const t = i / SR;
        ph += (2 * Math.PI * (45 + 90 * Math.exp(-t * 30))) / SR;
        add(start + i, 0.5 * v * Math.sin(ph) * Math.exp(-t * 12));
      }
    } else if (e.role === "clap" || e.role === "hat") {
      const dec = e.role === "clap" ? 14 : 60, amp = e.role === "clap" ? 0.18 : 0.06;
      let hp = 0, last = 0;
      for (let i = 0; i < 0.25 * SR; i++) {
        const t = i / SR, x = noise() * 2 - 1;
        hp = 0.7 * (hp + x - last); last = x;
        const env = e.role === "clap" ? Math.exp(-t * dec) * (t < 0.03 ? 0.6 + 0.4 * Math.sin(t * 700) : 1) : Math.exp(-t * dec);
        add(start + i, amp * v * hp * env);
      }
    } else if (e.role === "bass") {
      const n = Math.round((gate + 0.1) * SR);
      let ph = 0, lp = 0;
      for (let i = 0; i < n; i++) {
        const t = i / SR;
        ph = (ph + f / SR) % 1;
        const cut = e.sustain ? 350 : 200 + 1400 * Math.exp(-t * 18);
        lp += (1 - Math.exp((-2 * Math.PI * cut) / SR)) * (2 * ph - 1 - lp);
        const env = Math.min(1, t * (e.sustain ? 20 : 400)) * (t < gate * 0.9 ? 1 : Math.exp(-(t - gate * 0.9) * (e.sustain ? 8 : 60)));
        add(start + i, (e.sustain ? 0.25 : 0.32) * v * lp * env);
      }
    } else if (e.role === "stab" || e.role === "pad") {
      const pad = e.role === "pad";
      const atk = pad ? (arr.attack ?? 1.5) : 0.004, rel = pad ? (arr.attack ? 0.15 : 2.5) : 0.18;
      const n = Math.round((gate + rel * 3) * SR);
      const det = [0.995, 1, 1.005], ph = [0, 0.33, 0.66];
      let lp = 0;
      for (let i = 0; i < n; i++) {
        const t = i / SR;
        let s = 0;
        for (let k = 0; k < 3; k++) { ph[k] = (ph[k] + (f * det[k]) / SR) % 1; s += 2 * ph[k] - 1; }
        const cut = pad ? 1200 : 900 + 2600 * Math.exp(-t * 14);
        lp += (1 - Math.exp((-2 * Math.PI * cut) / SR)) * (s / 3 - lp);
        const env = Math.min(1, t / atk) * (t < gate ? (pad ? 1 : 0.55 + 0.45 * Math.exp(-t * 10)) : Math.exp(-(t - gate) / rel) * (pad ? 1 : 0.55));
        add(start + i, (pad ? 0.11 : 0.12) * v * lp * env);
      }
    } else if (e.role === "bell") {
      for (let i = 0; i < 2.0 * SR; i++) {
        const t = i / SR;
        const s = Math.sin(2 * Math.PI * f * t) + 0.3 * Math.sin(2 * Math.PI * f * 2.0 * t) * Math.exp(-t * 4);
        add(start + i, 0.07 * v * s * Math.min(1, t * 300) * Math.exp(-t * 2.2));
      }
    }
  }
  if (arr.delay) { // cheap feedback delay so ambient pads aren't bone dry
    const D = Math.round(0.375 * 60 / arr.bpm * 4 * SR / 2);
    for (let i = D; i < total; i++) buf[i] += 0.38 * buf[i - D];
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
function track(evs) {
  evs.sort((a, b) => a.tick - b.tick || a.order - b.order);
  const data = [];
  let last = 0;
  for (const e of evs) { data.push(...vlq(e.tick - last), ...e.bytes); last = e.tick; }
  data.push(0, 0xff, 0x2f, 0);
  const h = Buffer.alloc(8); h.write("MTrk", 0); h.writeUInt32BE(data.length, 4);
  return Buffer.concat([h, Buffer.from(data)]);
}
const text = (type, s) => [0xff, type, ...vlq(Buffer.byteLength(s)), ...Buffer.from(s)];
function midiFile(arr) {
  const T = 120;
  const us = Math.round(60e6 / arr.bpm);
  const tracks = [track([{ tick: 0, order: 0, bytes: [0xff, 0x51, 3, (us >> 16) & 255, (us >> 8) & 255, us & 255] },
    { tick: 0, order: 0, bytes: [0xff, 0x58, 4, 4, 2, 24, 8] },
    ...arr.markers.map((m) => ({ tick: m.pos * T, order: 1, bytes: text(0x06, m.text) }))])];
  const groups = [["bass", ["bass"], 0], ["chords", ["stab", "pad"], 1], ["bell", ["bell"], 2], ["drums", ["kick", "clap", "hat"], 9]];
  for (const [name, roles, ch] of groups) {
    const list = arr.events.filter((x) => roles.includes(x.role));
    if (!list.length) continue;
    const evs = [{ tick: 0, order: 0, bytes: text(0x03, name) }];
    for (const e of list) {
      evs.push({ tick: e.pos * T, order: 2, bytes: [0x90 | ch, e.midi, e.vel] });
      evs.push({ tick: (e.pos + e.len) * T - 1, order: 1, bytes: [0x80 | ch, e.midi, 0] });
    }
    tracks.push(track(evs));
  }
  const h = Buffer.alloc(14);
  h.write("MThd", 0); h.writeUInt32BE(6, 4); h.writeUInt16BE(1, 8); h.writeUInt16BE(tracks.length, 10); h.writeUInt16BE(480, 12);
  return Buffer.concat([h, ...tracks]);
}

// ---------------------------------------------------------------- write
let count = 0;
function emit(base, arr) {
  fs.writeFileSync(path.join(OUT, base + ".wav"), render(arr));
  fs.writeFileSync(path.join(OUT, base + ".mid"), midiFile(arr));
  count++;
  process.stdout.write(`\r${count} files…`);
  return base;
}
const SEEDS = [1, 2, 3];
// spc = seconds per chord, so the page can highlight the chord that is playing.
const player = (base, spc, extra = "") => `<audio controls preload="none" src="${base}.wav" data-spc="${spc}"${extra}></audio> <a href="${base}.mid">mid</a>`;

// Chord chips: red = diminished, amber = augmented/odd, green border = this move is in the scale's colour row.
function chips(sc, seq, chords) {
  const col = COLOUR[sc.id];
  return seq.map((d, i) => {
    const q = quality(sc, d);
    const cls = [q === "dim" ? "dim" : q === "aug" || q === "odd" ? "odd" : "",
      i && col && col.edges.some(([a, b]) => a === seq[i - 1] && b === d) ? "colmove" : ""].join(" ");
    return `<span class="chip ${cls}" title="${chordName(sc, d, chords[i].kind)}">${roman(sc, d)}</span>`;
  }).join("");
}
const strictDiffers = (sc) => [0, 1, 2, 3, 4, 5, 6].some((d) => guarded(sc, d, false) !== guarded(sc, d, true));

let html = `<!doctype html><meta charset="utf-8"><title>PROTOTYPE: chord graph across scales</title>
<style>body{font:14px system-ui;margin:2em;max-width:1250px}table{border-collapse:collapse;margin:.5em 0 1.2em}td,th{border:1px solid #ccc;padding:3px 7px;text-align:center;vertical-align:middle}
audio{width:210px;height:30px;vertical-align:middle}h2{margin-top:2.2em;border-top:2px solid #333;padding-top:.5em}code{background:#eee;padding:0 3px}
.chip{display:inline-block;min-width:2.2em;padding:1px 3px;margin:1px;border:2px solid #ddd;border-radius:4px;font-size:12px}.dim{background:#fbb}.odd{background:#ffe0a0}.colmove{border-color:#2a2}
.chip.now,td.now{outline:3px solid #06f;outline-offset:1px;font-weight:bold}
td.l{text-align:left}small{color:#666}</style>
<h1>PROTOTYPE — one chord graph across the 7-note scales</h1>
<p>Throwaway. Ticket: <a href="https://github.com/gismya/deluge-generative/issues/31">Chord graph across a genre's scales</a>. Key C throughout. Chord names hover on each chip.
<span class="chip dim">dim</span> diminished triad, <span class="chip odd">odd</span> augmented / major ♭5 / non-tertian, <span class="chip colmove">x</span> green border = a colour move.</p>
<p><b>Walk</b> (PERF-1 idea, hand-written weights): weight 1 + tonic pull + graph edge (+ colour edge), A-B-A allowed. Cadence target +20 on the last chord of a phrase (home not allowed there); every phrase starts home.
<b>guard</b>: ×0.15 on triads with a tritone fifth, guarded cadence target → V, else IV. <b>colour</b>: guard + the scale's colour row. <b>strict</b>: guard widened to every triad that isn't plain major or minor (only rendered where it differs).
Same seeds in every scale and variant, and every variant draws the same random numbers, so walks differ only where the weights do.</p>
<p><b>Suggested order</b> (≈15 min): 1) the scale tours of Locrian, Hungarian Minor, Marva, Arabian. 2) House seed 1 in Minor, Dorian, Major, then Dorian guard vs colour. 3) House Phrygian and Harmonic Minor, guard vs colour (vs strict). 4) Ambient Lydian and Dorian, guard vs colour. 5) Anything else that looks suspicious in the chips.</p>`;

// 1. tours
html += `<h2>1. Scale tours</h2><p>Each degree for one bar at 110 BPM: triad for two beats, then its 7th chord, then home. Pad + root bass.</p>
<table><tr><th>scale</th><th>listen</th>${ROMAN.map((r) => `<th>${r}</th>`).join("")}<th>guard catches</th><th>strict catches</th></tr>`;
for (const sc of SCALES) {
  const base = emit(`tour-${sc.id}`, tourArrangement(sc));
  const cells = [0, 1, 2, 3, 4, 5, 6].map((d) => {
    const q = quality(sc, d);
    return `<td class="deg ${q === "dim" ? "dim" : q === "aug" || q === "odd" ? "odd" : ""}">${roman(sc, d)}<br><small>${chordName(sc, d, 3)} · ${chordName(sc, d, 7)}</small></td>`;
  }).join("");
  const caught = (strict) => [0, 1, 2, 3, 4, 5, 6].filter((d) => guarded(sc, d, strict)).map((d) => roman(sc, d)).join(" ") || "—";
  html += `<tr><td class="l"><b>${sc.name}</b></td><td>${player(base, (4 * 60) / 110, " data-tour")}</td>${cells}<td>${caught(false)}</td><td>${caught(true)}</td></tr>`;
}
html += `</table>`;

// 2/3. graph renders
function graphSection(num, graph, arrange, scales, nChords, blurb) {
  html += `<h2>${num}. ${graph.name} graph</h2><p>${blurb}</p>
<p><small>tonic pull ${graph.tonic}, cadence every ${graph.cadEvery} chords to ${ROMAN[graph.cadTo]}, harmonic rhythm ${graph.hr} bars, ${graph.bpm} BPM. Edges: ${graph.edges.map(([a, b, w]) => `${ROMAN[a]}→${ROMAN[b]} +${w}`).join(", ")}.</small></p>
<table><tr><th>scale</th><th>variant</th>${SEEDS.map((s) => `<th>seed ${s}</th>`).join("")}</tr>`;
  for (const id of scales) {
    const sc = byId[id];
    const variants = ["guard", ...(COLOUR[id] ? ["colour"] : []), ...(strictDiffers(sc) ? ["strict"] : [])];
    variants.forEach((v, vi) => {
      html += `<tr>${vi === 0 ? `<td class="l" rowspan="${variants.length}"><b>${sc.name}</b></td>` : ""}<td>${v}${v === "colour" ? `<br><small>${COLOUR[id].label}</small>` : ""}</td>`;
      for (const seed of SEEDS) {
        const seq = walk(graph, sc, v, seed, nChords);
        const arr = arrange(sc, seq, seed);
        const base = emit(`${graph.name.toLowerCase()}-${id}-${v}-s${seed}`, arr);
        html += `<td class="l">${player(base, (graph.hr * 4 * 60) / graph.bpm)}<br>${chips(sc, seq, arr.chords)}</td>`;
      }
      html += `</tr>`;
    });
  }
  html += `</table>`;
}
graphSection("2", HOUSE, houseArrangement, SCALES.map((s) => s.id), 9,
  `All 12 scales through one House-style graph. 8 chords (one phrase) + the home chord, 2 bars each, ~36 s. 7th or 9th (1-3-7-9) stabs, offbeat root bass, kick/clap/hat.`);
graphSection("3", AMBIENT, ambientArrangement, ["major", "lydian", "dorian", "mixolydian"], 9,
  `Ambient-style graph in the ambient scales (Mixolydian added for comparison). Two 4-chord phrases + home. <b>Harmonic rhythm compressed to 4 bars</b> (the brief says 8+) to keep files ~2 min. Slow pad 7th chords, root drone, sparse bells on chord tones, a feedback delay.`);

// Highlight the chord that is playing; starting one player pauses the others.
html += `<script>
const players = [...document.querySelectorAll("audio[data-spc]")];
function targets(a) {
  return a.hasAttribute("data-tour") ? [...a.closest("tr").querySelectorAll("td.deg")] : [...a.closest("td").querySelectorAll(".chip")];
}
function mark(a) {
  const t = targets(a), i = Math.floor(a.currentTime / +a.dataset.spc);
  const k = a.hasAttribute("data-tour") ? i % 7 : Math.min(i, t.length - 1); // tour ends back on I
  t.forEach((el, j) => el.classList.toggle("now", j === k && (!a.paused || a.currentTime > 0)));
}
function clear(a) { targets(a).forEach((el) => el.classList.remove("now")); }
for (const a of players) {
  let raf = 0;
  const loop = () => { mark(a); raf = requestAnimationFrame(loop); };
  a.addEventListener("play", () => { for (const b of players) if (b !== a && !b.paused) b.pause(); cancelAnimationFrame(raf); loop(); });
  a.addEventListener("pause", () => { cancelAnimationFrame(raf); mark(a); });
  a.addEventListener("seeked", () => mark(a));
  a.addEventListener("ended", () => { cancelAnimationFrame(raf); clear(a); });
}
</script>`;
fs.writeFileSync(path.join(OUT, "index.html"), html);
console.log(`\nopen ${path.join(OUT, "index.html")}`);
