"""Which runner captures what, in which order, and what it costs.

Each runner has its own worktree and build dir. A capture whose (variant, Recipe) differs from the last one in its
build dir relinks the firmware (about 55 s under LTO); mutes and length don't. So a variant's Captures all go to one
runner, sorted by Recipe, and the runner applies the variant once for them.
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
    """The links a build dir makes running these Captures in order: one per change of (variant, Recipe), the first
    included."""
    n, last = 0, None
    for c in captures:
        if (c.variant, c.recipe) != last:
            n += 1
            last = (c.variant, c.recipe)
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
                f"runner {r.index}: {len(r.captures)} captures, {r.links} link{'s' * (r.links != 1)}, {r.audio_seconds:.1f} s of audio, "
                f"~{r.estimate_s / 60:.0f} min; variants: {', '.join(variants) or '-'}"
            )
            for c in r.captures:
                serves = (
                    f" (serves {', '.join(c.serves[1:])})" if len(c.serves) > 1 else ""
                )
                lines.append(
                    f"  {c.name}: {c.variant}, {r.seconds[c.name]:g} s{serves}"
                )
        total = sum(r.audio_seconds for r in self.runners)
        lines.append(
            f"total: {sum(len(r.captures) for r in self.runners)} captures, "
            f"{sum(r.links for r in self.runners)} links, {total:.1f} s captured; "
            f"~{max((r.estimate_s for r in self.runners), default=0) / 60:.0f} min wall"
        )
        return "\n".join(lines)


def groups(captures, secs):
    """[(variant, its Captures in order)], the costliest first. `secs` maps a capture's name to its seconds."""
    by_variant = {}
    for c in order(captures):
        by_variant.setdefault(c.variant, []).append(c)

    def cost(cs):
        return links(cs) * LINK_S + sum(
            CAPTURE_S + secs[c.name] * PER_AUDIO_S for c in cs
        )

    return sorted(by_variant.items(), key=lambda kv: (-cost(kv[1]), kv[0]))


def plan(captures, runners, tempo_of):
    """Each variant's Captures to one runner, the costliest variant first to the least loaded runner, as the runners
    claim them. `tempo_of(c)` is the harness render's tempo for Capture c."""
    secs = {c.name: seconds(c.bars, tempo_of(c)) for c in captures}
    out = [RunnerPlan(i + 1) for i in range(runners)]
    for _, cs in groups(captures, secs):
        r = min(out, key=lambda r: (r.estimate_s, r.index))
        r.captures += cs
        r.seconds.update({c.name: secs[c.name] for c in cs})
    return Plan(out)
