"""The Recipe check: before any capture, the harness render of each job's Recipe must play what the job asked for.

A runner that mangles a job's arguments (ambient rules 1 cut `--key D#` to `--key D`) captures the wrong piece, and
`gen_emu.py check` passes it, since it renders the same wrong arguments. This renders one bar of each job's Recipe
with generator_render (30 ms) and compares the THEORY track's `recipe` and `key` lines with the job's own flags.
"""

import theory_review

from . import jobs as _jobs
from .jobs import gen_emu


def mismatches(job, piece):
    """What the harness render `piece` (theory_review.Piece) plays differently from what `job` asked for, one line
    each; [] if nothing. Only what the job gives is checked: the rest is the genre's roll."""
    r, asked, out = piece.recipe, job.recipe, []

    def differs(name, want, have, same):
        if want is not None and not same:
            out.append(f"{name}: asked {want}, the harness plays {have}")

    differs("genre", asked.genre, r.get("genre"), r.get("genre") == asked.genre)
    for name in ("seed", "entropy"):
        value = getattr(asked, name)
        differs(name, value, r.get(name), r.get(name) == str(value))
    for name in gen_emu.MACROS:
        value = getattr(asked, name)
        if value is not None:
            differs(name, value, r.get(name), _float(r.get(name)) == float(value))
    key = piece.keys[0] if piece.keys else None
    if asked.key is not None:
        have = None if key is None else theory_review.PITCH_NAMES[key.root]
        differs(
            "key",
            asked.key,
            have,
            key is not None and key.root == gen_emu.KEYS[asked.key],
        )
    if asked.scale is not None:
        have = None if key is None else key.scale
        differs("scale", asked.scale, have, have == asked.scale)
    if asked.tempo is not None:
        have = r.get("tempo")
        differs(
            "tempo",
            f"{asked.tempo:g}",
            have,
            _float(have) is not None
            and round(_float(have) * 100) == gen_emu.hundredths(asked.tempo),
        )
    if asked.swing is not None:
        differs(
            "swing", asked.swing, r.get("swing"), r.get("swing") == str(asked.swing)
        )
    for variant in asked.variant:
        name, want = variant.split("=", 1)
        have = r.get("variant." + name)
        differs("variant " + name, want, have, have == want)
    return out


def _float(text):
    try:
        return float(text)
    except (TypeError, ValueError):
        return None


def harness_piece(job, render):
    """One bar of the harness render of `job`'s Recipe, as a theory_review.Piece."""
    return gen_emu.render_piece(render, job.recipe, 1)


def check(job_list, render):
    """{job name: tempo} for these Jobs, from the harness render. Raises RecipeMismatch listing every Job whose render
    plays something it didn't ask for."""
    tempos, bad = {}, []
    for job in job_list:
        piece = harness_piece(job, render)
        for line in mismatches(job, piece):
            bad.append(f"{job.name} (line {job.line}: {job.args}): {line}")
        tempos[job.name] = float(piece.recipe["tempo"])
    if bad:
        raise RecipeMismatch(
            "the Recipe check failed; nothing was captured:\n  " + "\n  ".join(bad)
        )
    return tempos


class RecipeMismatch(_jobs.JobError):
    pass
