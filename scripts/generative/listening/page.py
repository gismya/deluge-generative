"""The page: cards from a content file, clips cut from the scratch folder's captures and renders, index.html and
variants.md in the session folder.

A content file is a Python file defining SESSION, a dict:

    genre, round, folder      the Export's opening line: "Listening-session answers (<genre>, round <round>,
                              <folder>): handle them with the listening-session skill."
    title, intro              the page's heading, and its opening paragraphs (a list)
    sections                  [{id, title}], in page order
    cards                     [card], in page order; a card:
        id, ticket, title     "S1", "#133", "The Arp's figure"
        section               a section's id
        now                   what plays today, one or two sentences
        options               [{key, label, variant}]: label says what changes, in musical terms; variant names
                              its diff (or None)
        listen                one line: the part, and where in the clip
        setup                 the Recipe and bars, small print
        clips                 [{stem, src, bars: [first, end), label, kind}]: src is a job name (a capture in
                              raw/, or an offline render); kind: "full" (default) or "isolated"
        rough                 True: rough render (stand-in sounds), badged
        normal_mode, picks, question, midi   optional, as earlier sessions used them; midi is [{label, variant,
                              args, bars}] rendered by bin/<variant> --midi-only
        genre                 optional: the genre its MIDI renders in (default the session's; a midi entry's
                              args may give --genre too), for a session comparing genres (#426)
        verdicts              optional, with picks: the answers each clip takes, {name: [answer, ...]} in
                              order, as {"level": ["louder", "fine", "quieter"], "weight": ["0", "1", "2"]};
                              default {"verdict": ["keep", "unsure", "drop"]}
    later                     [{title, ticket, waits}]: held for a later round
    not_renderable            [{title, ticket, why}]
    details                   optional {card id: Markdown}: what the card stood for, for variants.md
    variants_intro            optional Markdown for variants.md's top: the base commit, how to apply a diff

Card text is short (SKILL.md step 4): `words()` counts it, and the build warns past 60.
"""

import importlib.util
import json
import os
import re
import shlex
import subprocess

from . import cutter, variants
from . import jobs as _jobs

HERE = os.path.dirname(os.path.abspath(__file__))
CARD_FIELDS = ("id", "ticket", "title", "section", "now", "listen", "setup", "clips")
MAX_WORDS = 60
# A picks card's answers per clip, unless it names its own.
VERDICTS = {"verdict": ["keep", "unsure", "drop"]}


def load(path):
    spec = importlib.util.spec_from_file_location("listening_content", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.SESSION


def header(session):
    return (
        f"Listening-session answers ({session['genre']}, round {session['round']}, {session['folder']}): "
        "handle them with the listening-session skill."
    )


def words(card):
    text = " ".join(
        [card.get("now", ""), card.get("listen", ""), card.get("question") or ""]
        + [o["label"] for o in card.get("options", [])]
    )
    return len(text.split())


def problems(session, sources):
    """What's wrong with a content file, one line each. `sources` holds the names clips may cut from."""
    out = []
    for f in ("genre", "round", "folder", "title", "sections", "cards"):
        if not session.get(f):
            out.append(f"session: no {f}")
    sections = {s["id"] for s in session.get("sections", [])}
    ids = set()
    for card in session.get("cards", []):
        cid = card.get("id", "?")
        if cid in ids:
            out.append(f"{cid}: id repeated")
        ids.add(cid)
        out += [
            f"{cid}: no {f}" for f in CARD_FIELDS if f != "clips" and not card.get(f)
        ]
        if card.get("section") and card["section"] not in sections:
            out.append(f"{cid}: section {card['section']} isn't in sections")
        if card.get("genre") and card["genre"] not in _jobs._names(
            "genres", _jobs.gen_emu.genre_names
        ):
            out.append(f"{cid}: unknown genre {card['genre']}")
        out += verdict_problems(cid, card.get("verdicts", VERDICTS))
        keys = [o.get("key") for o in card.get("options", [])]
        if len(set(keys)) != len(keys):
            out.append(f"{cid}: option keys repeated")
        stems = set()
        for clip in card.get("clips", []):
            stem = clip.get("stem", "?")
            if stem in stems:
                out.append(f"{cid} clip {stem}: stem repeated")
            stems.add(stem)
            if clip.get("src") not in sources:
                out.append(
                    f"{cid} clip {stem}: no capture or render named {clip.get('src')}"
                )
            bars = clip.get("bars") or []
            if len(bars) != 2 or not bars[0] < bars[1]:
                out.append(
                    f"{cid} clip {stem}: bars must be [first, end) with first < end"
                )
    return out


def verdict_problems(cid, verdicts):
    """What's wrong with a card's `verdicts`: it must name at least one answer list, each of distinct answers."""
    if not isinstance(verdicts, dict) or not verdicts:
        return [f"{cid}: verdicts must be {{name: [answer, ...]}}"]
    out = []
    for name, answers in verdicts.items():
        if (
            not isinstance(answers, list)
            or not answers
            or len(set(answers)) != len(answers)
        ):
            out.append(f"{cid}: verdicts {name}: a list of distinct answers")
    return out


def page_data(session, clips):
    """The page's DATA: the content with each clip's file and seconds (`clips`: stem -> seconds)."""
    cards = []
    for card in session["cards"]:
        c = dict(card)
        c.setdefault("options", [])
        c.setdefault("verdicts", VERDICTS)
        c["clips"] = [
            dict(
                x,
                file=f"audio/{x['stem']}.m4a",
                seconds=clips.get(x["stem"], 0),
                kind=x.get("kind", "full"),
            )
            for x in card["clips"]
        ]
        c["midiFiles"] = c.pop("midiFiles", [])
        c.pop("midi", None)
        cards.append(c)
    return {
        "session": f"{session['genre']}-round-{session['round']}",
        "title": session["title"],
        "intro": session.get("intro", []),
        "sections": session["sections"],
        "header": header(session),
        "cards": cards,
        "later": session.get("later", []),
        "notRenderable": session.get("not_renderable", []),
    }


def html(data):
    with open(os.path.join(HERE, "page.html")) as fh:
        template = fh.read()
    # </script> inside a string would end the script: JSON escapes "/" when told to via the replace.
    return template.replace(
        "/*DATA*/null", json.dumps(data, ensure_ascii=False).replace("</", "<\\/")
    )


def source_info(scratch, name):
    """A capture's or render's sidecar: {tempo, seconds, variant, args, kind}."""
    with open(os.path.join(scratch, "raw", name + ".json")) as fh:
        return json.load(fh)


def sources(scratch):
    raw = os.path.join(scratch, "raw")
    if not os.path.isdir(raw):
        return set()
    return {
        f[:-4]
        for f in os.listdir(raw)
        if f.endswith(".wav") and os.path.exists(os.path.join(raw, f[:-4] + ".json"))
    }


def render_midi(card, scratch, out_dir, genre):
    """The card's MIDI files, each in its genre: the entry's own --genre, else the card's `genre`, else its variant's
    recorded genre (diffs/genres.json in the session folder `out_dir`), else `genre` (the session's)."""
    variant_genres = variants.genres(out_dir)
    files = []
    for m in card.get("midi", []):
        slug = re.sub(r"[^a-z0-9]+", "-", m["label"].lower()).strip("-")
        name = f"{card['id'].lower()}-{slug}"
        dest = os.path.join(out_dir, "midi", name)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        args = shlex.split(m["args"])
        if not any(a == "--genre" or a.startswith("--genre=") for a in args):
            args = [
                "--genre",
                card.get("genre") or variant_genres.get(m.get("variant")) or genre,
                *args,
            ]
        subprocess.run(
            [
                os.path.join(scratch, "bin", m.get("variant") or "base"),
                *args,
                "--bars",
                str(m.get("bars", 200)),
                "--midi-only",
                "--out",
                dest,
            ],
            check=True,
            capture_output=True,
        )
        files.append({"label": m["label"], "file": f"midi/{name}.mid"})
    return files


def cut_key(scratch, clip):
    """What a clip's m4a was cut from: its source, bars, and the source WAV's identity (inode, size, mtime) and
    sidecar. A capture taken again, or new bars, changes it, and build() cuts the clip again."""
    raw = os.path.join(scratch, "raw", clip["src"])
    st = os.stat(raw + ".wav")
    with open(raw + ".json") as fh:
        sidecar = fh.read()
    return [
        clip["src"],
        list(clip["bars"]),
        st.st_ino,
        st.st_size,
        st.st_mtime_ns,
        sidecar,
    ]


def clip_warnings(card_id, clip, info):
    """What to read before a clip goes on the page: it is cut short, or its capture failed gen_emu's check twice with
    the same bytes and was kept (runner.py), so its silence may be a fault, not the music."""
    out = []
    end = clip["bars"][1]
    if end * 240.0 / info["tempo"] > info["seconds"] + 1e-6:
        out.append(
            f"{card_id} clip {clip['stem']}: bar {end} is past {clip['src']}'s "
            f"{info['seconds']:g} s; cut short"
        )
    if info.get("check"):
        out.append(
            f"{card_id} clip {clip['stem']}: {clip['src']} was kept with its check {info['check']}; listen to it, "
            "or give its job --quiet-bars for the bars that are silent on purpose"
        )
    return out


def build(session, scratch, out_dir, recut=False):
    """Cuts every clip, renders the MIDI files, writes index.html and variants.md. Returns warning lines; raises
    ValueError listing the content file's problems."""
    found = problems(session, sources(scratch))
    if found:
        raise ValueError("the content file:\n  " + "\n  ".join(found))
    warnings = [
        f"{c['id']}: {words(c)} words (at most {MAX_WORDS})"
        for c in session["cards"]
        if words(c) > MAX_WORDS
    ]
    seconds = {}
    index_path = os.path.join(out_dir, "audio", "cuts.json")
    try:
        with open(index_path) as fh:
            cuts = json.load(fh)
    except (OSError, ValueError):
        cuts = {}
    for card in session["cards"]:
        for clip in card["clips"]:
            info = source_info(scratch, clip["src"])
            dest = os.path.join(out_dir, "audio", clip["stem"] + ".m4a")
            first, end = clip["bars"]
            warnings += clip_warnings(card["id"], clip, info)
            key = cut_key(scratch, clip)
            done = cuts.get(clip["stem"])
            if recut or not os.path.exists(dest) or not done or done["key"] != key:
                cut_s = cutter.cut(
                    os.path.join(scratch, "raw", clip["src"] + ".wav"),
                    dest,
                    info["tempo"],
                    first,
                    end,
                    scratch=scratch,
                )
                cuts[clip["stem"]] = done = {"key": key, "seconds": round(cut_s, 1)}
                os.makedirs(os.path.dirname(index_path), exist_ok=True)
                with open(index_path, "w") as fh:
                    json.dump(cuts, fh, indent=1)
            seconds[clip["stem"]] = done["seconds"]
        card["midiFiles"] = render_midi(card, scratch, out_dir, session["genre"])
    data = page_data(session, seconds)
    with open(os.path.join(out_dir, "index.html"), "w") as fh:
        fh.write(html(data))
    with open(os.path.join(out_dir, "variants.md"), "w") as fh:
        fh.write(variants_md(session, scratch, out_dir))
    return warnings


def variants_md(session, scratch, out_dir):
    """Each variant the page uses: where, and its exact diff; then each card's details."""
    used = {}
    for card in session["cards"]:
        for o in card.get("options", []):
            if o.get("variant"):
                used.setdefault(o["variant"], []).append(
                    f"{card['id']} ({card['ticket']}) option {o['key']}: {o['label']}"
                )
        for clip in card["clips"]:
            v = source_info(scratch, clip["src"]).get("variant", "base")
            if v != "base":
                used.setdefault(v, []).append(f"{card['id']} clip {clip['stem']}")
    lines = [f"# {session['title']}: variants", ""]
    if session.get("variants_intro"):
        lines += [session["variants_intro"], ""]
    for v in sorted(used):
        lines += [f"## {v}", ""] + [f"- {u}" for u in dict.fromkeys(used[v])] + [""]
        path = variants.diff_path(out_dir, v)
        if os.path.exists(path):
            with open(path) as fh:
                lines += ["```diff", fh.read().rstrip(), "```", ""]
        else:
            lines += ["(no diff in diffs/)", ""]
    details = session.get("details") or {}
    if details:
        lines += ["# Card details", ""]
        for card in session["cards"]:
            if card["id"] in details:
                lines += [
                    f"## {card['id']} {card['title']} ({card['ticket']})",
                    "",
                    details[card["id"]].strip(),
                    "",
                ]
    return "\n".join(lines)
