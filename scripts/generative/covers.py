"""Which probe scripts a change needs, from their "# covers:" lines (#314).

    python3 scripts/generative/covers.py [--base BRANCH] [--full] [--hardware]
    python3 scripts/generative/covers.py --check

A probe script names the paths it covers in "# covers: PATH ..." lines (repo-relative, more than one line allowed). A
PATH is a file, a folder (every file under it) or a glob (fnmatch: `*` also crosses `/`). A script runs for a change
when any changed file:
  - is one of its covers paths, or under one;
  - is the script itself or a file it includes (perform_open.txt and the like, followed through their includes);
  - is one of runner_files(): test_port_emu.py, deluge_probe.py, the scripts/generative/ modules they import and the
    scripts those name (perform_leave.txt, which start_state runs), the pinned delugemu and the firmware's test port. Those change what every script does.
A script with no "# covers:" line runs whenever any script runs (fail safe), and test_port_smoke.txt always does.
A FIRMWARE path (src/, lib/, the root CMakeLists.txt, the linker script, the toolchain and its CMake file
(`scripts/cmake/`), the dbt build tasks) or what smoke and the leak check run on (gen_emu.py, its imports and
perform_mem_cycle.txt with its includes) or what decides the leg (check_all.py, covers.py) builds the firmware and runs
delugemu whatever the covers lines pick.

check_all.py picks its delugemu scripts with it, unless --full or --scripts. The CLI prints the scripts a hardware
session or a delugemu run needs for the changes since the merge base with origin/BASE (pr_base.py, as check_all.py
finds it), one repo-relative path a line, test_port_smoke.txt first: --hardware lists the scripts a Deluge can run
(scripts/generative/*.txt with a start_state, but not those whose "# needs:" line names a delugemu card, gen_emu.py or
someone listening); without it, the scripts check_all.py runs in delugemu. --full lists every one. --check fails
naming each covers path that matches no file, each src/ file that sends an event a script expects (or rules out)
that its covers lines miss (a string literal whose first word is the event's kind), and each event expect that doesn't
start with its kind (check_all.py runs it every time;
so do the unit tests).
"""

import argparse
import fnmatch
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

COVERS = re.compile(r"^#\s*covers:(.*)$", re.MULTILINE)
INCLUDE = re.compile(r"^\s*include\s+(\S.*?)\s*(?:#.*)?$", re.MULTILINE)
START_STATE = re.compile(r"^start_state\b", re.MULTILINE)
IMPORT = re.compile(r"^\s*(?:import\s+([\w, ]+)|from\s+(\w+)\s+import)", re.MULTILINE)
# An event a script waits on, rules out or keeps to compare: expect [next] [no_]event TEXT, keep events TEXT; TEXT
# plain or /regex/, then the line's options.
EXPECT_LINE = re.compile(
    r"^(?:expect (?:next )?(?:no_)?event|keep events) (.*?)(?: (?:since=mark|within=\S+))*\s*$",
    re.MULTILINE,
)
SMOKE = "scripts/generative/test_port_smoke.txt"
SCRIPT_LITERAL = re.compile(r"[\"']([\w-]+\.txt)[\"']")
# The firmware's inputs: a change to one of these (covers paths: a file, a folder, a glob) builds the firmware and runs
# delugemu, with smoke, the leak check and test_port_smoke.txt. The dbt tasks a build runs are among them.
FIRMWARE = (
    "src/",
    "lib/",
    "CMakeLists.txt",
    "*.ld",
    "toolchain/",
    "scripts/toolchain/",
    "dbt",
    "dbt.py",
    "scripts/tasks/task-build.py",
    "scripts/tasks/task-configure.py",
    "scripts/tasks/util.py",
    "scripts/cmake/",  # the root CMakeLists.txt's CMAKE_TOOLCHAIN_FILE: the compiler and the ARM flags
    "scripts/generative/step_section.py",  # reads the Release build: runs only with it
)
# What every script runs on, besides the scripts/generative/ modules test_port_emu.py and deluge_probe.py import
# (runner_files()): a change to one runs every script.
EVERY_SCRIPT_TOO = (
    "scripts/generative/delugemu.pin",
    "src/deluge/io/midi/test_port.cpp",
    "src/deluge/io/midi/test_port.h",
)
# The events whose text a TEST_EVENT("gen_x %s", ...) fills from other files, and those files (covers paths): a
# script that expects the text checks them too (">KEY" in gen_status is queueTag() in queue_text.cpp). () where the
# sender builds the text itself (a ternary, its own snprintf, a path off the card). Every kind with a %s sender has an
# entry (builder_gaps(), which check() runs). The Acid techno table (techno.cpp) names the genre and parts every script
# checks. Nothing can see a helper in the sending file that reads its text from elsewhere (l10n, a genre table,
# another file's names): upperControlName() reads l10n for gen_edit and gen_controls, OutputSink::partName() the
# genre's part names for gen_h16. Such a kind lists those files by hand.
_GENRE = ("src/deluge/generative/core/genres/techno.cpp",)
_SECTIONS = ("src/deluge/generative/core/types.h",)  # sectionName()
_L10N = ("src/deluge/gui/l10n/english.json", "src/deluge/gui/l10n/g_english.cpp")
_SCREEN = (
    "src/deluge/generative/perform/oled_page.*",
    "src/deluge/generative/perform/queue_text.*",
    "src/deluge/generative/perform/text.*",
)
BUILDERS = {
    "gen_arcmap": ("src/deluge/generative/perform/arc_map.*",),
    "gen_arcpage": (*_SCREEN, "src/deluge/generative/perform/arc_map.*"),
    "gen_arcrow": ("src/deluge/generative/perform/arc_map.*",),
    "gen_browser": (),
    "gen_browser_preview": (),  # the previewed path
    "gen_log": (),  # at=stop or at=phrase, literals beside it
    "gen_clip": ("src/deluge/generative/perform/clip_page.*",),
    "gen_performance_pad": (),  # down or up
    "gen_punch": (
        "src/deluge/generative/perform/keyboard_page.*",
    ),  # describePunches()
    "gen_punch_pad": (),  # down or up
    "gen_compose": (),  # the outcome's name
    "gen_compose_pick": (
        *_GENRE,
        "src/deluge/generative/core/lane_names.h",
    ),  # a part's or laneName()'s row
    "gen_compose_recipe": _GENRE,  # the genre's name
    "gen_compose_seed": ("src/deluge/generative/perform/clip_page.*",),  # seedCode()
    "gen_compose_song": (
        *_GENRE,
        "src/deluge/generative/perform/clip_page.*",
    ),  # name=GEN <oledName> <seedCode()>
    "gen_confirm": _GENRE,
    "gen_controls": _L10N,  # " BRI=..": upperControlName() reads l10n
    "gen_delay_setups": _GENRE,  # the genre's name
    "gen_edit": _L10N,  # "control=BRI": upperControlName()
    "gen_editor": (),
    "gen_entry": (),
    "gen_faders": ("src/deluge/generative/perform/clip_page.*",),
    "gen_fill_button": (),
    "gen_from_menu": (),  # the list's name
    "gen_generator_heap": (),  # the heap's name
    "gen_h16": (
        *_GENRE,
        "src/deluge/generative/firmware/output_sink.cpp",
    ),  # culled=: OutputSink::partName()
    "gen_h2": ("src/deluge/generative/firmware/perform_adapter.cpp",),  # the ramps tag
    "gen_key": (),
    "gen_key_set": (
        "src/deluge/generative/perform/perform_steering.h",
    ),  # keySetName()
    "gen_keyboard": ("src/deluge/generative/perform/keyboard_page.*",),
    "gen_lane_list": (
        "src/deluge/generative/core/palette_file.*",
    ),  # lanePaletteName()
    "gen_leave_menu": (),
    "gen_macropage": _SCREEN,
    "gen_mem": (  # the callers' tags: before_entry, after_leave, after_free, before_sound, before_transition
        "src/deluge/generative/firmware/entry_leave.cpp",
        "src/deluge/generative/firmware/retirements.cpp",
        "src/deluge/generative/firmware/sound_swaps.cpp",
        "src/deluge/generative/firmware/transitions.cpp",
    ),
    "gen_mark": ("src/deluge/generative/perform/capture.*",),  # the MarkOutcome's name
    "gen_menu": (),
    "gen_menu_cancel_change": (),
    "gen_octave_landed": (),
    "gen_octave_popup": (
        "src/deluge/generative/perform/queue_text.*",
        *_L10N,
    ),  # queueLabel(), LOWEST|HIGHEST
    "gen_page": (),
    "gen_page_leds": (),
    "gen_palette_override": _GENRE,
    "gen_partpage": (
        *_SCREEN,
        "src/deluge/generative/perform/clip_page.*",
    ),  # takeLabel()
    "gen_peek": _SCREEN,
    "gen_peek_page": (),
    "gen_perform_entered": _GENRE,
    "gen_press": _SECTIONS,  # "gen_press force GROOVE"
    "gen_recipe_roll": (),  # the roll's and the outcome's names
    "gen_recipe_load": _GENRE,  # file names from the card, genre=<stable name>, older|newer
    "gen_recipe_save": (
        *_GENRE,
        "src/deluge/generative/perform/clip_page.*",
        "src/deluge/generative/perform/recipe_name.*",
    ),  # quick|full, and name=<oledName> <seedCode()>[letter]
    "gen_restart": (),  # as_it_is or as_it_began
    "gen_restart_preload": (),  # how AS IT BEGAN's preload ended
    "gen_record_led": (
        "src/deluge/generative/perform/capture.*",
    ),  # the RecordLed's name
    "gen_restart_press": (),  # what the press did
    "gen_retire_referenced": (),
    "gen_rings": (),
    "gen_row_level": _GENRE,  # part and lane names
    "gen_scale_led": (),
    "gen_section": _SECTIONS,
    "gen_seed_popup": (
        "src/deluge/generative/perform/clip_page.*",  # seedCode()
        "src/deluge/generative/perform/queue_text.*",  # keyName(), scaleName()
    ),
    "gen_sidebar": ("src/deluge/generative/perform/sidebar.*",),
    "gen_sound": (*_GENRE, "src/deluge/generative/core/lane_names.h"),  # laneName()
    "gen_sound_delay": _GENRE,  # part and lane names
    "gen_sound_level": _GENRE,
    "gen_sound_preload": (
        "src/deluge/generative/perform/sound_swap_lifecycle.h",
    ),  # sourceName()
    "gen_sound_transpose": (
        *_GENRE,
        "src/deluge/generative/core/lane_names.h",
    ),  # laneName()
    "gen_status": _SCREEN,
    "gen_tempo_popup": (
        "src/deluge/playback/playback_handler.cpp",  # getTempoStringForOLED()
        *_L10N,  # "Tempo:", STRING_FOR_EXTERNAL_CLOCK_TAG's "EXT"
    ),
    "gen_transition": (
        "src/deluge/generative/perform/transition_lifecycle.h",
    ),  # traceText(), phaseName()
    "gen_transition_preload": (),
}
# A file whose literals hold events another file sends whole (TEST_EVENT("%s", text)), and that sender: every kind the
# builder holds is sent by it too (builder_gaps(), which check() runs: every TEST_EVENT("%s" file is one).
RELAYS = {
    "src/deluge/generative/perform/sound_swap_lifecycle.h": (
        "src/deluge/generative/firmware/sound_swaps.*",
    ),
}
# "# needs:" text a hardware session can't give: a delugemu card overlay, the leak check's runner, a listener.
NOT_ON_HARDWARE = re.compile(r"^# needs: (card |gen_emu\.py|.*listening)", re.MULTILINE)


def git_lines(*args, repo=REPO):
    """git's output lines, or None if it failed: a caller must not read a failure as an empty answer."""
    proc = subprocess.run(
        ["git", *args], cwd=repo, capture_output=True, text=True, check=False
    )
    return None if proc.returncode else [n for n in proc.stdout.split("\n") if n]


def changed_paths(merge_base, repo=REPO):
    """Every path changed since merge_base, deleted and untracked ones included, repo-relative; None if git failed
    (callers then run everything: a failed diff is not "nothing changed")."""
    # --no-renames: a moved file lists its old path too, which a covers line may name.
    diff = git_lines("diff", "--name-only", "--no-renames", merge_base, repo=repo)
    untracked = git_lines("ls-files", "--others", "--exclude-standard", repo=repo)
    if diff is None or untracked is None:
        return None
    return sorted(set(diff + untracked))


def repo_files(repo=REPO):
    """Every file in the repo, tracked or untracked but not ignored, repo-relative (a walk if git fails)."""
    files = git_lines(
        "ls-files", "--cached", "--others", "--exclude-standard", repo=repo
    )
    if files is not None:
        return files
    return [
        rel(os.path.join(folder, name), repo)
        for folder, _dirs, names in os.walk(repo)
        for name in names
        if "/.git/" not in folder + "/"
    ]


def rel(path, repo=REPO):
    return os.path.relpath(os.path.abspath(path), repo).replace(os.sep, "/")


def module_files(*roots, here=HERE, repo=REPO):
    """The scripts/generative/ modules `roots` (file names) are and import, followed through their imports,
    repo-relative: from their import lines, so the list can't drift from the code."""
    seen, todo = set(), list(roots)
    while todo:
        name = todo.pop()
        path = os.path.join(here, name)
        if name in seen or not os.path.isfile(path):
            continue
        seen.add(name)
        with open(path) as fh:
            for plain, frm in IMPORT.findall(fh.read()):
                for module in plain.split(",") if plain else [frm]:
                    todo.append(module.strip().split(" ")[0] + ".py")
    return {rel(os.path.join(here, n), repo) for n in seen}


def module_scripts(modules, here=HERE, repo=REPO):
    """The probe scripts the modules (repo-relative) name as string literals ("perform_leave.txt", which start_state
    runs; "perform_mem_cycle.txt", the leak check's cycle) and the files those include, repo-relative. Not
    test_port_smoke.txt, which test_port_emu.py only defaults to."""
    found = set()
    for module in modules:
        with open(os.path.join(repo, module)) as fh:
            for name in SCRIPT_LITERAL.findall(fh.read()):
                path = os.path.join(here, name)
                if name != os.path.basename(SMOKE) and os.path.isfile(path):
                    found |= own_files(path, repo)
    return found


def runner_files(here=HERE, repo=REPO):
    """What every probe script runs on: test_port_emu.py, deluge_probe.py, the modules they import and the scripts
    those name (module_scripts()), and EVERY_SCRIPT_TOO."""
    modules = module_files("test_port_emu.py", "deluge_probe.py", here=here, repo=repo)
    return modules | module_scripts(modules, here, repo) | set(EVERY_SCRIPT_TOO)


def emulator_files(here=HERE, repo=REPO, every=None):
    """What smoke and the leak check run on: gen_emu.py, the modules it imports and the scripts those name
    (perform_mem_cycle.txt and its includes), check_all.py and covers.py (they pick what the leg runs), and
    runner_files() (`every`, if the caller has it)."""
    # check_all.py and covers.py decide what the emulator leg runs: a change to them runs it
    modules = module_files("gen_emu.py", here=here, repo=repo) | {
        rel(os.path.join(here, n), repo)
        for n in ("check_all.py", "covers.py")
        if os.path.isfile(os.path.join(here, n))
    }
    if every is None:
        every = runner_files(here, repo)
    return modules | module_scripts(modules, here, repo) | every


def script_text(path):
    with open(path) as fh:
        return fh.read()


def covers(text):
    """The paths of a script's "# covers:" lines, or None if it has none."""
    lines = COVERS.findall(text)
    if not lines:
        return None
    return [p for line in lines for p in line.split()]


def script_covers(path):
    """covers() of the script at `path`."""
    return covers(script_text(path))


def matches(pattern, path):
    """Whether the repo-relative `path` is the covers path `pattern`, under it (a folder) or matched by it (a glob)."""
    folder = pattern.rstrip("/")
    return (
        path == folder
        or path.startswith(folder + "/")
        or fnmatch.fnmatchcase(path, pattern)
    )


def own_files(path, repo=REPO, depth=0):
    """The script at `path` and every file it includes, followed through their includes, repo-relative."""
    if depth > 8:
        raise ValueError(f"{path}: includes nested too deep")
    found = {rel(path, repo)}
    for name in INCLUDE.findall(script_text(path)):
        included = os.path.join(os.path.dirname(path), name)
        if os.path.isfile(included):
            found |= own_files(included, repo, depth + 1)
    return found


def picked_by(path, changed, repo=REPO, every=None, paths=None):
    """Whether the change (repo-relative paths) needs the script at `path` by its covers paths, its own files or
    runner_files() (`every`, if the caller has it). A script with no covers line is picked only by its own files and
    runner_files() here: it runs anyway once anything does (pick()). `paths`: its covers paths, if read already."""
    changed = set(changed)
    if every is None:
        every = runner_files(os.path.join(repo, "scripts", "generative"), repo)
    if changed & every or changed & own_files(path, repo):
        return True
    if paths is None:
        paths = script_covers(path)
    return any(matches(p, f) for p in paths or [] for f in changed)


def firmware_changed(changed, repo=REPO, every=None):
    """Whether the change needs the firmware built and delugemu run whatever scripts it picks: a FIRMWARE path, or
    what smoke and the leak check run on. `every`: runner_files(), if the caller has it."""
    here = os.path.join(repo, "scripts", "generative")
    emulator = emulator_files(here, repo, every)
    return any(f in emulator or any(matches(d, f) for d in FIRMWARE) for f in changed)


def pick(scripts, changed, repo=REPO):
    """The scripts (paths) the change needs, in their order: none if the firmware didn't change and no script is
    picked by its paths; else those picked and every one with no covers line."""
    every = runner_files(os.path.join(repo, "scripts", "generative"), repo)
    paths = {s: script_covers(s) for s in scripts}
    by_change = [s for s in scripts if picked_by(s, changed, repo, every, paths[s])]
    if not by_change and not firmware_changed(changed, repo, every):
        return []
    return [s for s in scripts if s in by_change or paths[s] is None]


def missing(scripts, repo=REPO):
    """(script name, covers path) for each covers path that matches no file in the repo, by matches() (what pick()
    uses): a renamed or deleted file leaves a script that never runs for the code that replaced it."""
    files = repo_files(repo)
    out = []
    for s in scripts:
        for p in script_covers(s) or []:
            if not any(matches(p, f) for f in files):
                out.append((os.path.basename(s), p))
    return out


def expect_texts(path):
    """(file name:line, TEXT) for each event expect in the script at `path` and its includes."""
    out = []
    folder = os.path.dirname(path)
    for name in sorted(own_files(path, folder)):
        for number, line in enumerate(
            script_text(os.path.join(folder, name)).splitlines(), 1
        ):
            m = EXPECT_LINE.match(line)
            if m:
                out.append((f"{os.path.basename(name)}:{number}", m.group(1)))
    return out


def expect_kinds(text, sent):
    """The kinds of sent event (keys of `sent`) an expect's TEXT can match, or None when its first word is none of
    them (TEXT starts with a field, "force=B", so nothing ties it to a sender: give it its kind). A plain TEXT that is
    the first word alone ("gen_h1"), or a regex whose kind no space follows (/^gen_h1/), matches longer kinds too, so
    every kind it begins counts (gen_h16 too)."""
    regex = len(text) > 1 and text.startswith("/") and text.endswith("/")
    body = text[1:-1].lstrip("^") if regex else text
    word = re.match(r"[a-z][a-z0-9_]*", body)
    if not word:
        return None
    kind = word.group(0)
    if not body[len(kind) :].startswith(" "):
        # nothing ends the kind ("gen_h1", /gen_h1/, /^gen_h1\d*/): it matches the longer kinds it begins too
        return {k for k in sent if k.startswith(kind)} or None
    return {kind} if kind in sent else None


def expected_events(path, sent):
    """The kinds of event the script at `path` and its includes expect, rule out or keep (expect_kinds())."""
    kinds = set()
    for _, text in expect_texts(path):
        kinds |= expect_kinds(text, sent) or set()
    return kinds


def kindless(scripts, repo=REPO, sent=None):
    """(where, TEXT) for each event expect whose TEXT starts with no sent kind: it ties to no sender, and a plain one
    can match inside any event ("EXT" in "NEXT BREAK"). Each must start with its kind, as TEXT or /^kind .../."""
    if sent is None:
        sent = emitters(repo)
    out = []
    for s in scripts:
        for where, text in expect_texts(s):
            if expect_kinds(text, sent) is None:
                out.append((where, text))
    return sorted(set(out))


# What src_lines() greps for: an event kind as a literal's first word (emitters()), a TEST_EVENT that fills its text
# with %s (builder_gaps()'s kinds) and one that sends text built elsewhere (its relays).
EMITS = r'"[a-z][a-z0-9]*_[a-z0-9_]+[ "%]'
FILLS = r'TEST_EVENT\("[a-z][a-z0-9_]* [^"]*%s'
RELAYED = r'TEST_EVENT\("%s"'


def src_lines(repo=REPO):
    """[(path, line)] for each line under src/ that emitters() or builder_gaps() reads: one git grep for both, with
    untracked files (a new src/ file not yet added counts)."""
    out = []
    for hit in _grep_src(f"{EMITS}|{FILLS}|{RELAYED}", repo, "-n").splitlines():
        path, _, rest = hit.partition(":")
        out.append((path, rest.partition(":")[2]))
    return out


def emitters(repo=REPO, lines=None):
    """{event kind: the files under src/ that have it as a string literal's first word}: where the firmware sends it
    (TEST_EVENT("gen_phrase bar=%ld"), out.add("gen_sound_swap part=")). Read with git grep, untracked files too
    (src_lines(), or `lines` already read). A file that only sends text another file builds (TEST_EVENT("%s",
    perform::traceText(...)) in sound_swaps.cpp and transitions.cpp) is invisible here: the builder
    (sound_swap_lifecycle.h, transition_lifecycle.h) stands for it, so a covers line naming the builder needs its
    sender beside it by hand."""
    found = {}
    for path, line in src_lines(repo) if lines is None else lines:
        for kind in re.findall(r'"([a-z][a-z0-9]*_[a-z0-9_]+)[ "%]', line):
            found.setdefault(kind, set()).add(path)
    return found


def _grep_src(pattern, repo, *options):
    """git grep's output over src/, untracked files included; raises if git grep fails (1 is "no match")."""
    proc = subprocess.run(
        ["git", "grep", "--untracked", *options, "-E", pattern, "--", "src"],
        cwd=repo,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode > 1:
        raise RuntimeError(f"git grep failed in {repo}: {proc.stderr.strip()}")
    return proc.stdout


def builder_gaps(repo=REPO, builders=None, relays=None, lines=None):
    """What BUILDERS and RELAYS miss: each kind a TEST_EVENT("gen_x ... %s") fills with no BUILDERS entry, and each
    file with a TEST_EVENT("%s") no RELAYS entry names. Run by check(), so a new sender under src/ is caught on every
    check_all run, not only when the unit tests run. It sees a %s only on the TEST_EVENT( line itself: a format
    string continued on the next line is missed. A grep that finds neither kind of sender is a gap too: the firmware
    has both, so the pattern or git grep broke."""
    builders = BUILDERS if builders is None else builders
    relays = RELAYS if relays is None else relays
    lines = src_lines(repo) if lines is None else lines
    kinds = {
        k
        for _, line in lines
        for k in re.findall(r'TEST_EVENT\("([a-z][a-z0-9_]*) [^"]*%s', line)
    }
    relaying = sorted({path for path, line in lines if re.search(RELAYED, line)})
    out = []
    if not kinds:
        out.append(
            'the grep found no TEST_EVENT("gen_x ... %s") under src/: check covers.FILLS and git grep'
        )
    if not relaying:
        out.append(
            'the grep found no TEST_EVENT("%s", ...) under src/: check covers.RELAYED and git grep'
        )
    out += [
        f"{k}: TEST_EVENT fills it with %s and covers.BUILDERS has no entry"
        for k in sorted(kinds - set(builders))
    ]
    relayed = [p for senders in relays.values() for p in senders]
    for path in relaying:
        if not any(matches(p, path) for p in relayed):
            out.append(
                f"{path}: sends text built elsewhere and covers.RELAYS doesn't name it"
            )
    return out


def senders(repo=REPO, builders=None, relays=None, lines=None):
    """{event kind: files}: emitters(), plus the files that build each kind's text (BUILDERS) and the files that send
    a builder's events whole (RELAYS), as repo files."""
    builders = BUILDERS if builders is None else builders
    relays = RELAYS if relays is None else relays
    sent = emitters(repo, lines)
    files = repo_files(repo)

    def resolve(patterns):
        return {f for f in files for p in patterns if matches(p, f)}

    for kind, patterns in builders.items():
        if kind in sent:
            sent[kind] |= resolve(patterns)
    for builder, patterns in relays.items():
        relay = resolve(patterns)
        for kind in sent:
            if builder in sent[kind]:
                sent[kind] |= relay
    return sent


def uncovered(scripts, repo=REPO, builders=None, sent=None):
    """(script name, event kind, file) for each file that sends or builds an event a script expects (senders()) but
    that neither its covers lines nor runner_files() name. A script with no covers line runs with every firmware
    change: none for it."""
    if sent is None:
        sent = senders(repo, builders)
    here = os.path.join(repo, "scripts", "generative")
    every = runner_files(here, repo)
    out = []
    for s in scripts:
        paths = script_covers(s)
        if paths is None:
            continue
        for kind in sorted(expected_events(s, sent)):
            for f in sorted(sent.get(kind, ())):
                if f not in every and not any(matches(p, f) for p in paths):
                    out.append((os.path.basename(s), kind, f))
    return out


def check(scripts, repo=REPO):
    """Prints each covers path that matches no file and each file that sends an event a script expects but its
    covers lines miss, and how many scripts have covers lines. Returns 1 if any."""
    gone = missing(scripts, repo)
    for name, path in gone:
        print(f"{name}: {path} matches no file: update the covers line")
    lines = src_lines(repo)  # one grep of src/ for every check below
    sent = senders(repo, lines=lines)
    unseen = uncovered(scripts, repo, sent=sent)
    for name, kind, path in unseen:
        print(
            f"{name}: expects {kind}, which {path} sends or builds: add it to the covers lines"
        )
    loose = kindless(scripts, repo, sent)
    gaps = builder_gaps(repo, lines=lines)
    for gap in gaps:
        print(gap)
    for where, text in loose:
        print(
            f"{where}: expect {text!r} starts with no event kind a src/ file sends: give it its kind"
        )
    with_lines = sum(script_covers(s) is not None for s in scripts)
    print(
        f"{with_lines} of {len(scripts)} scripts have covers lines, {len(gone)} paths missing, "
        f"{len(unseen)} event senders uncovered, {len(loose)} expects without a kind, {len(gaps)} builder gaps"
    )
    return 1 if gone or unseen or loose or gaps else 0


def all_scripts(here=HERE):
    """Every scripts/generative/*.txt that is a check of its own (has a start_state line; not an include)."""
    return [
        p
        for p in sorted(glob.glob(os.path.join(here, "*.txt")))
        if START_STATE.search(script_text(p))
    ]


def probe_scripts(here=HERE):
    """(scripts, skipped): test_port_smoke.txt, then every perform_*.txt and compose_*.txt that is a check on its own
    (not an include)
    and that delugemu can run (deluge_probe.script_needs(): no "# needs:" line but "# needs: card DIR"); and the names
    of the checks it can't run."""
    import deluge_probe

    scripts, skipped = [os.path.join(here, os.path.basename(SMOKE))], []
    paths = glob.glob(os.path.join(here, "perform_*.txt")) + glob.glob(
        os.path.join(here, "compose_*.txt")
    )
    for path in sorted(paths):
        text = script_text(path)
        if not START_STATE.search(text):
            continue  # an include (perform_open.txt and the like)
        if deluge_probe.script_needs(path, text).delugemu:
            scripts.append(path)
        else:
            skipped.append(os.path.basename(path))
    return scripts, skipped


def hardware_scripts(here=HERE):
    """The scripts a Deluge session can run: see the module docstring."""
    return [p for p in all_scripts(here) if not NOT_ON_HARDWARE.search(script_text(p))]


def main(argv=None):
    import pr_base

    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--base", help="as check_all.py's --base")
    p.add_argument("--full", action="store_true", help="every script, whatever changed")
    p.add_argument(
        "--hardware", action="store_true", help="the scripts a Deluge session can run"
    )
    p.add_argument(
        "--check",
        action="store_true",
        help="fail on a covers path that matches no file, an event sender a script's covers lines miss, an expect"
        " with no event kind, or a builder gap (a %%s-filled kind or a relaying file BUILDERS or RELAYS misses)",
    )
    args = p.parse_args(argv)
    if args.check:
        return check(all_scripts())
    smoke = os.path.join(REPO, SMOKE)
    if args.hardware:
        candidates = [s for s in hardware_scripts() if s != smoke]
    else:
        candidates = [s for s in probe_scripts()[0] if s != smoke]
    if args.full:
        scripts = candidates
    else:
        base = pr_base.merge_base(args.base, REPO)
        print(f"covers: {base.line()}", file=sys.stderr)
        if base.error or not base.merge_base:
            print("covers: no merge base, so every script", file=sys.stderr)
            scripts = candidates
        else:
            changed = changed_paths(base.merge_base)
            if changed is None:
                print("covers: git diff failed, so every script", file=sys.stderr)
                scripts = candidates
            else:
                scripts = pick(candidates, changed)
    for s in [smoke, *scripts]:
        print(rel(s))
    return 0


if __name__ == "__main__":
    sys.exit(main())
