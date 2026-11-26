"""Which runner captures what, in which order, and what it costs.

Each runner has its own worktree and build dir. Applying or reverting a variant changes the source, so the runner's
next capture relinks the firmware (about 55 s under LTO). A Recipe change builds nothing: gen_emu sends the Recipe
(variants included) over the test port (#523, #548); nor do mutes and length. So a variant's Captures go to one runner,
and the runner applies the variant once for them. They still run in Recipe order, so the plan reads in that order.

A variant costing more than a runner's share of the work (the base, as a rule) is split into chunks, `base#1`,
`base#2` ..., each claimed like a variant, when the extra links cost less than the wall time the split saves (#422).
"""

import math
from dataclasses import dataclass, field

# Costs on the M4 Pro (listening-speed audit, 2026-11-20): seconds.
LINK_S = 55.0
CAPTURE_S = 19.0
PER_AUDIO_S = 0.27
SPARE_S = 1.0  # captured past the last bar a clip needs


def seconds(bars, tempo):
    """Seconds to capture for `bars` bars at `tempo` BPM: rounded up to a tenth, and SPARE_S more."""
    return math.ceil(round(bars * 240.0 / tempo * 10, 6)) / 10 + SPARE_S


def order(captures):
    """Captures sorted by (variant, Recipe), base first, then by mutes, longest first: equal (variant, Recipe) run back
    to back."""
    return sorted(
        captures,
        key=lambda c: (
            c.variant != "base",
            c.variant,
            c.recipe.sort_key(),
            c.job.mutes,
            -c.bars,
        ),
    )


def links(captures):
    """The links a build dir makes running these Captures in order: one per change of variant, the first included (a
    runner reverts and applies before each variant's group, and moves its worktree to the repo's HEAD at the start)."""
    n, last = 0, None
    for c in captures:
        if c.variant != last:
            n += 1
            last = c.variant
    return n


@dataclass
class RunnerPlan:
    index: int
    captures: list = field(default_factory=list)
    seconds: dict = field(default_factory=dict)  # capture name -> seconds

    @property
    def links(self):
        return links(self.captures)

    @property
    def audio_seconds(self):
        return sum(self.seconds.values())

    @property
    def estimate_s(self):
        return (
            self.links * LINK_S
            + len(self.captures) * CAPTURE_S
            + self.audio_seconds * PER_AUDIO_S
        )


@dataclass
class Plan:
    runners: list

    def text(self):
        lines = []
        for r in self.runners:
            variants = list(dict.fromkeys(c.variant for c in r.captures))
            lines.append(
                f"runner {r.index}: {len(r.captures)} captures, {r.links} link{'s' * (r.links != 1)}, "
                f"{r.audio_seconds:.1f} s of audio, "
                f"~{r.estimate_s / 60:.0f} min; variants: {', '.join(variants) or '-'}"
            )
            for c in r.captures:
                serves = (
                    f" (serves {', '.join(c.serves[1:])})" if len(c.serves) > 1 else ""
                )
                lines.append(
                    f"  {c.name}: {c.variant}, {r.seconds[c.name]:g} s{serves}"
                )
        lines.append(
            "(an estimate: the runners claim whole variants, or chunks of a large one (base#1, base#2), costliest "
            "first, as each frees up, so the split can differ; the links and seconds per unit don't. A link per unit: "
            "a new Recipe builds nothing)"
        )
        total = sum(r.audio_seconds for r in self.runners)
        lines.append(
            f"total: {sum(len(r.captures) for r in self.runners)} captures, "
            f"{sum(r.links for r in self.runners)} links, {total:.1f} s captured; "
            f"~{max((r.estimate_s for r in self.runners), default=0) / 60:.0f} min wall"
        )
        return "\n".join(lines)


def cost(cs, secs):
    """A runner's seconds for Captures `cs` in this order: their links, captures and audio."""
    return links(cs) * LINK_S + sum(CAPTURE_S + secs[c.name] * PER_AUDIO_S for c in cs)


def chunks(cs, k, secs):
    """`cs` cut into `k` runs in their order, of about equal capture cost."""
    each = [CAPTURE_S + secs[c.name] * PER_AUDIO_S for c in cs]
    total, out, done = sum(each), [[] for _ in range(k)], 0.0
    for c, e in zip(cs, each):
        # the chunk this capture's middle falls in, and never past the last
        out[min(k - 1, int((done + e / 2) * k / total))].append(c)
        done += e
    return [chunk for chunk in out if chunk]


def wall(costs, runners):
    """The longest runner's seconds when the units of `costs` go, costliest first, to the least loaded runner."""
    loads = [0.0] * runners
    for c in sorted(costs, reverse=True):
        loads[loads.index(min(loads))] += c
    return max(loads)


def groups(captures, secs, runners=1):
    """[(unit, its Captures in order)], the costliest first. A unit is a variant's name, or `<variant>#<n>` for a chunk
    of a variant split across runners: a variant costing more than a runner's share of the work is split, costliest
    first, into the number of chunks (up to `runners`) that gives the shortest wall time, so a split is only made when
    the time it saves passes its extra links. `secs` maps a capture's name to its seconds."""
    by_variant = {}
    for c in order(captures):
        by_variant.setdefault(c.variant, []).append(c)
    split = dict.fromkeys(by_variant, 1)

    def units(split):
        out = []
        for v, cs in by_variant.items():
            parts = chunks(cs, split[v], secs) if split[v] > 1 else [cs]
            out += [
                (v if len(parts) == 1 else f"{v}#{i}", part)
                for i, part in enumerate(parts, 1)
            ]
        return out

    share = sum(cost(cs, secs) for cs in by_variant.values()) / runners
    for v in sorted(by_variant, key=lambda v: (-cost(by_variant[v], secs), v)):
        if cost(by_variant[v], secs) <= share:
            continue  # less than a runner's share: a split would only trade links for a few captures
        best = None
        for k in range(1, min(runners, len(by_variant[v])) + 1):
            w = wall([cost(cs, secs) for _, cs in units({**split, v: k})], runners)
            if best is None or w < best[0] - 1e-6:
                best = (w, k)
        split[v] = best[1]
    return sorted(units(split), key=lambda kv: (-cost(kv[1], secs), kv[0]))


def plan(captures, runners, tempo_of):
    """Each unit's Captures (a variant's, or a chunk of one: groups()) to one runner, the costliest first to the least
    loaded runner, as the runners claim them. `tempo_of(c)` is the harness render's tempo for Capture c."""
    secs = {c.name: seconds(c.bars, tempo_of(c)) for c in captures}
    out = [RunnerPlan(i + 1) for i in range(runners)]
    for _, cs in groups(captures, secs, runners):
        r = min(out, key=lambda r: (r.estimate_s, r.index))
        r.captures += cs
        r.seconds.update({c.name: secs[c.name] for c in cs})
    return Plan(out)
