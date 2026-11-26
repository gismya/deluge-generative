"""Offline clips: the harness rough render (generator_render's WAV), per variant, for cards about the notes only.

Kit rows play their real samples; pitched parts play one stand-in voice, every sound at home. A job line is the same
as a capture's (`name | variant | card | bars | args`); the card column is ignored (write `-`). Each variant renders
with <scratch>/bin/<variant>, the generator_render `variants.py prove` kept for it. --mute needs a generator_render
that takes it (#402).
"""

import concurrent.futures
import math
import os
import subprocess
import tempfile

from . import jobs as _jobs
from . import runner as _runner
from . import variants as _variants


def takes_mute(render):
    done = subprocess.run(
        [render, "--help"], capture_output=True, text=True, check=False
    )
    return "--mute" in (done.stdout + done.stderr)


def render_one(capture, render, raw, tempo):
    """Renders `capture`'s Recipe for its bars (and one more) to raw/<name>.wav, and its sidecar."""
    bars = math.ceil(capture.bars) + 1
    with tempfile.TemporaryDirectory(dir=raw) as tmp:
        prefix = os.path.join(tmp, "render")
        cmd = [
            render,
            *capture.job.capture_flags(),
            "--bars",
            str(bars),
            "--out",
            prefix,
        ]
        done = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if done.returncode != 0 or not os.path.exists(prefix + ".wav"):
            raise RuntimeError(
                f"{capture.name}: {' '.join(cmd)}: {(done.stderr or done.stdout).strip()[-300:]}"
            )
        final = os.path.join(raw, capture.name + ".wav")
        os.replace(prefix + ".wav", final)
    for other in capture.serves[1:]:
        link = os.path.join(raw, other + ".wav")
        if os.path.exists(link):
            os.replace(link, link + ".old")
        os.link(final, link)
    _runner.write_info(raw, capture, tempo, bars * 240.0 / tempo, "rough render")
    return capture.name


def render_all(job_list, scratch, session, base_render=None):
    """Renders every job (merged by prefix), all at once. Returns the names rendered."""
    from . import recipe

    raw = os.path.join(scratch, "raw")
    os.makedirs(raw, exist_ok=True)
    bins = os.path.join(scratch, "bin")
    base = base_render or os.path.join(bins, "base")
    if not os.path.exists(base):
        base = _jobs.gen_emu.theory_review.find_render()
    tempos = recipe.check(job_list, base)
    captures = _jobs.merge(job_list)
    renders = {}
    for c in captures:
        try:
            render = _variants.current_render(
                _runner.REPO, scratch, session, c.variant, fallback_base=False
            )
        except _jobs.JobError as e:
            raise _jobs.JobError(f"{c.name}: {e}") from None
        if c.job.mutes and not takes_mute(render):
            raise _jobs.JobError(
                f"{c.name}: --mute needs a generator_render that takes it (#402)"
            )
        renders[c.name] = render
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        return list(
            pool.map(
                lambda c: render_one(c, renders[c.name], raw, tempos[c.name]), captures
            )
        )
