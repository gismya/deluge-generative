"""Print the context size of each recently active agent session on this repo.

    python3 scripts/agents/agent_context.py [-n N] [--match TEXT]

For docs/agents/orchestration.md's "Fresh context past 450k tokens" rule: a session's context is its latest call's
input tokens plus its cache tokens (read and written), from its Claude Code transcript. One line per session, newest
first: when it was last active, the context in thousands of tokens, the calls it made, and its working directory.
Match a session to its T3 thread by that directory (the thread's worktree) and the time.

Transcripts are read from ~/.claude/projects/ ($CLAUDE_CONFIG_DIR/projects/ when that's set), in the project dirs
whose name contains --match (default: deluge-generative, which covers every worktree of this repo). Sub-agent
(sidechain) calls are left out: they don't add to the parent's context.
"""

import argparse
import glob
import json
import os
from datetime import datetime, timezone


def projects_dir():
    root = os.environ.get("CLAUDE_CONFIG_DIR") or os.path.join("~", ".claude")
    return os.path.join(os.path.expanduser(root), "projects")


def session_context(path):
    """(context tokens, calls, cwd) of a transcript's latest main-thread call, or None when it made none."""
    last, calls, cwd = None, 0, "?"
    with open(path, errors="replace") as f:
        for line in f:
            try:
                entry = json.loads(line)
            except ValueError:
                continue
            if not isinstance(entry, dict):
                continue
            cwd = entry.get("cwd", cwd)
            if entry.get("type") != "assistant" or entry.get("isSidechain"):
                continue
            usage = (entry.get("message") or {}).get("usage")
            if usage:
                last = usage
                calls += 1
    if not last:
        return None
    tokens = sum(
        last.get(key) or 0
        for key in (
            "input_tokens",
            "cache_read_input_tokens",
            "cache_creation_input_tokens",
        )
    )
    return tokens, calls, cwd


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("-n", type=int, default=8, help="sessions to show (default 8)")
    parser.add_argument(
        "--match",
        default="deluge-generative",
        help="text in the project dir's name (default: deluge-generative)",
    )
    args = parser.parse_args(argv)
    pattern = os.path.join(projects_dir(), f"*{glob.escape(args.match)}*", "*.jsonl")
    files = sorted(glob.glob(pattern), key=os.path.getmtime, reverse=True)
    shown = 0
    for path in files:
        if shown >= args.n:
            break
        found = session_context(path)
        if not found:
            continue
        tokens, calls, cwd = found
        when = (
            datetime.fromtimestamp(os.path.getmtime(path), tz=timezone.utc)
            .astimezone()
            .strftime("%m-%d %H:%M")
        )
        print(f"{when}  ctx {tokens // 1000:>4}k  calls {calls:>4}  {cwd}")
        shown += 1
    if not shown:
        print(f"no sessions in {pattern}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
