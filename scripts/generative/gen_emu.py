"""Generative mode in delugemu (spec §21.3, §21.5): smoke checks and the genre tuning capture.

    python3 scripts/generative/gen_emu.py smoke [--bars 8] [--no-leak]
    python3 scripts/generative/gen_emu.py leak [--cycles 4]
    python3 scripts/generative/gen_emu.py capture --genre techno --seed 7 --energy 40 --seconds 60

smoke    boots a local `./dbt build release` (build/Release/deluge.elf), retrying a blank boot, enters Perform through
         SHIFT+SONG -> GENERATIVE -> PERFORM FROM -> GENRE -> the first genre -> DISCARD SONG? -> OK, presses PLAY and
         plays at least N bars (2 s a bar, so N bars at any tempo from 120 BPM). It fails on silence (any half second
         of digital silence in the firmware's rendered audio) or a freeze (the OLED, which shows the beat, not
         changing for a second), and checks that PLAY stops and that holding BACK, then LEAVE, leaves. Functional
         only: no audio hashes. Then runs the leak check below (skip it with --no-leak) and reports both: one
         failing doesn't hide the other.

leak     the memory check (spec §13.8: memory after leaving matches memory before entry). Builds the test-port
         firmware (build-testport/, Release with -DENABLE_SYSEX_TEST=ON; build/ keeps the default options), boots it
         and runs perform_mem_cycle.txt --cycles times on one boot over the test port, then reads the gen_mem
         before_entry / after_leave events. Fails unless, for every cycle after the first WARMUP_CYCLES, internal and
         external free memory after leaving equal their values before entry exactly.

capture  the tuning loop's main step: builds a capture firmware in build-capture/ whose Current Recipe and seed come
         from the arguments (genre, Recipe overrides, seed; the same flags as the harness's generator_render), boots
         it, plays Perform for --seconds and writes the rendered audio to a WAV, then plays it (afplay) unless
         --no-play. Deterministic: the same arguments give the same WAV.

Everything runs under icount (-icount shift=2,sleep=off -S): the VM only moves in run_until steps, every input goes in
at a fixed virtual time through button() and every wait counts virtual time. pylib's wall-clock helpers (press,
load_song, wait_until, wait_sd_idle) are never used.

The delugemu the checks last passed with is pinned in delugemu.pin beside this script. A run warns when the installed
bundle's VERSION or the delugemu checkout (which provides pylib and run.sh) differs. `smoke --update-pin` records
what is installed once the checks pass on it; commit that on purpose.

Needs a delugemu source checkout (~/source/delugemu), its nightly bundle's QEMU and the factory card image
(~/source/delugemu-sd/deluge_sd.img, attached with -snapshot so it is never written).
"""

import argparse
import hashlib
import math
import os
import re
import shlex
import struct
import subprocess
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deluge_probe
import test_port_emu

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
HOME = os.path.expanduser("~")
PIN_PATH = os.path.join(HERE, "delugemu.pin")
QEMU = os.path.join(
    HOME,
    "source",
    "delugemu-nightly",
    "DelugEmu-macos-arm64",
    "DelugEmu.app",
    "Contents",
    "MacOS",
    "qemu-system-arm",
)

RATE = 44100
FRAME = 8  # S32LE stereo, as DELUGEMU_SSIF_DUMP writes it
MS = 1_000_000
SEC = 1_000_000_000
STEP = 100 * MS  # how far the VM runs between looks at it
SECONDS_PER_BAR = 2.0  # 4 beats at 120 BPM, the slowest tempo a genre rolls
SILENCE_RMS = 1e-4  # of full scale; the firmware's idle output is exact zeros
SILENCE_WINDOW = 0.5  # s: a 4/4 genre at 120 BPM or faster has a beat in every one
FREEZE_WINDOW = (
    SEC  # the Perform OLED shows the beat, so it changes at least twice a second
)

# The leak check (spec §13.8).
LEAK_CYCLES = 4
# The first cycle enters from the boot song, whose teardown and first loads may warm caches; perform_runner.txt has
# always called the second gen_mem pair the one that must match. Cycles after it must return both heaps exactly (#167
# fixed the stock Clear song leak that a 104 B external allowance used to cover). A change of a few bytes either way is
# still a finding: the heap layout repeats exactly when everything is freed, so a moved block means a held one.
WARMUP_CYCLES = 1
GEN_MEM = re.compile(
    r"gen_mem (before_entry|after_leave) internal=(\d+) external=(\d+)"
)

KEYS = {"C": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3, "E": 4, "F": 5, "F#": 6, "Gb": 6, "G": 7, "G#": 8,
        "Ab": 8, "A": 9, "A#": 10, "Bb": 10, "B": 11}  # fmt: skip
MACROS = ("energy", "density", "complexity", "motion")


def log(msg):
    print("[gen-emu] " + msg, flush=True)


# -- checks ------------------------------------------------------------------


def audio_failures(pcm, seconds, window=SILENCE_WINDOW):
    """Why `seconds` of rendered S32LE stereo audio isn't a playing Perform: too short, or silent somewhere."""
    failures = []
    frames = len(pcm) // FRAME
    have = frames / RATE
    if have + 0.01 < seconds:
        failures.append(f"only {have:.1f} s of audio rendered, wanted {seconds:.1f} s")
    span = int(window * RATE)
    silent_from = None
    start = 0
    while (
        start + span <= min(frames, int(seconds * RATE) + span - 1) and start < frames
    ):
        chunk = pcm[start * FRAME : min(start + span, frames) * FRAME]
        left = struct.unpack(f"<{len(chunk) // 4}i", chunk)[0::2]
        rms = math.sqrt(sum(v * v for v in left[::4]) / max(1, len(left[::4]))) / 2**31
        if rms < SILENCE_RMS and silent_from is None:
            silent_from = start
        elif rms >= SILENCE_RMS and silent_from is not None:
            failures.append(
                f"silent from {silent_from / RATE:.1f} s to {start / RATE:.1f} s"
            )
            silent_from = None
        start += span
    if silent_from is not None:
        failures.append(
            f"silent from {silent_from / RATE:.1f} s to {min(start, frames) / RATE:.1f} s"
        )
    return failures


def freeze_failures(samples, window_ns=FREEZE_WINDOW):
    """Why OLED samples [(vclock_ns, frame)] aren't a live Perform: the frame stopped changing for `window_ns`."""
    if not samples:
        return ["no OLED samples"]
    t0 = samples[0][0]
    changed_at, last = t0, samples[0][1]
    for t, frame in samples[1:]:
        if frame != last:
            changed_at, last = t, frame
        elif t - changed_at >= window_ns:
            return [
                "the OLED stopped changing at %.1f s (frozen?)"
                % ((changed_at - t0) / SEC)
            ]
    return []


def mem_cycles(events):
    """Each completed enter/leave cycle's free memory, from gen_mem test events: [(before, after)], each a
    {"internal": B, "external": B} dict. An entry with no after_leave (cancelled, or failed) pairs with nothing."""
    cycles, before = [], None
    for line in events:
        m = GEN_MEM.search(line)
        if not m:
            continue
        sample = {"internal": int(m.group(2)), "external": int(m.group(3))}
        if m.group(1) == "before_entry":
            before = sample
        elif before is not None:
            cycles.append((before, sample))
            before = None
    return cycles


def leak_report(cycles, wanted, warmup=WARMUP_CYCLES):
    """(lines, failures) for the leak check: a line of deltas per cycle, and why it fails, if it does."""
    lines, failures = [], []
    for n, (before, after) in enumerate(cycles, 1):
        d_int = after["internal"] - before["internal"]
        d_ext = after["external"] - before["external"]
        checked = n > warmup
        lines.append(
            f"cycle {n}: internal {before['internal']} -> {after['internal']} ({d_int:+d} B), "
            f"external {before['external']} -> {after['external']} ({d_ext:+d} B)"
            + ("" if checked else " [warm-up, not checked]")
        )
        if not checked:
            continue
        for heap, delta in (("internal", d_int), ("external", d_ext)):
            if delta != 0:
                failures.append(
                    f"cycle {n}: {heap} free memory changed by {delta:+d} B across enter/leave; must be 0"
                )
    if len(cycles) < wanted:
        failures.append(
            f"only {len(cycles)} of {wanted} enter/leave cycles completed (see the script output)"
        )
    if len(cycles) <= warmup:
        failures.append(f"no cycle after the {warmup} warm-up cycle(s) to check")
    return lines, failures


# -- the delugemu pin --------------------------------------------------------


def read_pin(path=PIN_PATH):
    """The pinned delugemu, {} if nothing is pinned yet."""
    pin = {}
    if not os.path.exists(path):
        return pin
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if line and not line.startswith("#"):
                key, _, value = line.partition("=")
                pin[key.strip()] = value.strip()
    return pin


def write_pin(installed, path=PIN_PATH):
    with open(path, "w") as fh:
        fh.write(
            "# The delugemu the Generative checks last passed with (spec §21.3). gen_emu.py warns when the installed\n"
            "# one differs. Bump it on purpose: gen_emu.py smoke --update-pin, then commit.\n"
            "bundle={}\ncheckout={}\n".format(
                installed["bundle"], installed["checkout"]
            )
        )


def installed_versions(qemu, delugemu):
    version = os.path.join(
        os.path.dirname(os.path.dirname(qemu)), "Resources", "VERSION"
    )
    bundle = "unknown"
    if os.path.exists(version):
        with open(version) as fh:
            bundle = fh.read().strip()
    checkout = subprocess.run(
        ["git", "-C", delugemu, "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=False,
    ).stdout.strip()
    return {"bundle": bundle, "checkout": checkout or "unknown"}


def pin_warnings(pin, installed):
    if not pin:
        return [
            "no delugemu is pinned yet: run gen_emu.py smoke --update-pin once the checks pass"
        ]
    warnings = []
    if installed.get("bundle") != pin.get("bundle"):
        warnings.append(
            "the installed delugemu bundle is {}, but the checks last passed with {} (delugemu.pin)".format(
                installed.get("bundle"), pin.get("bundle")
            )
        )
    if installed.get("checkout") != pin.get("checkout"):
        warnings.append(
            "the delugemu checkout (pylib, run.sh) is at {}, but the checks last passed with {} (delugemu.pin)".format(
                installed.get("checkout"), pin.get("checkout")
            )
        )
    return warnings


# -- the capture build -------------------------------------------------------


def genre_names():
    """Each genre's stable name, from the genre tables."""
    folder = os.path.join(REPO, "src", "deluge", "generative", "core", "genres")
    names = []
    for name in sorted(os.listdir(folder)):
        if name.endswith(".cpp"):
            with open(os.path.join(folder, name)) as fh:
                names += re.findall(
                    r"(?:const|constexpr) Genre\s+\w+\s*\{\s*\.name\s*=\s*\"([^\"]+)\"",
                    fh.read(),
                )
    return names


def scale_names():
    """Scale values in order, as kebab-case names ("harmonic-minor"), from the core's Scale enum."""
    with open(
        os.path.join(REPO, "src", "deluge", "generative", "core", "types.h")
    ) as fh:
        body = re.search(
            r"enum class Scale : uint8_t \{(.*?)\};", fh.read(), re.DOTALL
        ).group(1)
    names = [n.strip() for n in body.split(",") if n.strip()]
    return [re.sub(r"(?<!^)(?=[A-Z])", "-", n).lower() for n in names]


def capture_header(args):
    """generative_capture.h for a capture build: a RecipeOverrides initialiser (generative/core/recipe_overrides.h)."""
    if args.genre not in genre_names():
        sys.exit(
            "unknown genre: {} (have {})".format(args.genre, ", ".join(genre_names()))
        )
    key = -1
    if args.key is not None:
        if args.key not in KEYS:
            sys.exit(f"unknown key: {args.key}")
        key = KEYS[args.key]
    scale = -1
    if args.scale is not None:
        if args.scale not in scale_names():
            sys.exit(
                "unknown scale: {} (have {})".format(
                    args.scale, ", ".join(scale_names())
                )
            )
        scale = scale_names().index(args.scale)
    macros = ", ".join(
        str(-1 if getattr(args, m) is None else getattr(args, m)) for m in MACROS
    )
    fields = [
        f'.genre = "{args.genre}"',
        f".entropy = {args.entropy}u",
        f".seed = {args.seed}u",
        f".macroPoints = {{{macros}}}",
        f".tempo = {0 if args.tempo is None else hundredths(args.tempo)}",
        f".keyRoot = {key}",
        f".scale = {scale}",
    ]
    if args.swing is not None:
        fields.append(f".swing = {args.swing}")
    return (
        "// Generated by scripts/generative/gen_emu.py capture. Not committed.\n"
        "#define GENERATIVE_CAPTURE_OVERRIDES {{{}}}\n".format(", ".join(fields))
    )


def uint32(text):
    """A seed or entropy, as generator_render takes it: 0 <= n < 2^32."""
    n = int(text, 0)
    if not 0 <= n < 2**32:
        raise argparse.ArgumentTypeError("takes a 32-bit number")
    return n


def bpm(text):
    """A tempo, as generator_render takes it: 20 to 999 BPM (NaN refused)."""
    value = float(text)
    if not (value >= 20 and value <= 999):
        raise argparse.ArgumentTypeError("takes 20 to 999 BPM")
    return value


def hundredths(tempo):
    """Hundredths of a BPM, rounded as generator_render rounds them."""
    return int(tempo * 100 + 0.5)


def capture_path(args, out):
    """out/<every argument>.wav, with -2, -3 ... added rather than overwrite an earlier capture."""
    name = f"{args.genre}-seed{args.seed}"
    if args.entropy:
        name += f"-entropy{args.entropy}"
    for m in MACROS:
        if getattr(args, m) is not None:
            name += f"-{m}{getattr(args, m)}"
    if args.key is not None:
        name += f"-key{args.key}"
    if args.scale is not None:
        name += f"-{args.scale}"
    if args.tempo is not None:
        name += f"-tempo{args.tempo:g}"
    if args.swing is not None:
        name += f"-swing{args.swing}"
    name += f"-{args.seconds:g}s"
    path = os.path.join(out, name + ".wav")
    n = 1
    while os.path.exists(path):
        n += 1
        path = os.path.join(out, f"{name}-{n}.wav")
    return path


def dbt(command):
    log("./dbt exec " + command)
    subprocess.run([os.path.join(REPO, "dbt"), "exec", command], cwd=REPO, check=True)


def build_capture(args):
    """Builds the capture firmware for these arguments. Incremental: only current_recipe.cpp rebuilds."""
    build = os.path.abspath(args.build_dir)
    gen = os.path.join(build, "generated")
    os.makedirs(gen, exist_ok=True)
    header = os.path.join(gen, "generative_capture.h")
    text = capture_header(args)
    old = None
    if os.path.exists(header):
        with open(header) as fh:
            old = fh.read()
    if old != text:
        with open(header, "w") as fh:
            fh.write(text)
    if not os.path.exists(os.path.join(build, "CMakeCache.txt")):
        dbt(
            f"cmake -B {shlex.quote(build)} -S {shlex.quote(REPO)} -G 'Ninja Multi-Config' -DCMAKE_CROSS_CONFIGS:STRING=all "
            f"'-DCMAKE_DEFAULT_CONFIGS=Debug;Release' -DGENERATIVE_CAPTURE_DIR={shlex.quote(gen)}"
        )
    dbt(f"cmake --build {shlex.quote(build)} --config Release --target deluge")
    return os.path.join(build, "Release", "deluge.elf")


# -- driving the emulator ----------------------------------------------------


class Emu:
    """One delugemu run under icount. The VM moves only in run_to(); every input lands at a fixed virtual time."""

    def __init__(self, args, elf, out, tag):
        sys.path.insert(0, os.path.join(args.delugemu, "pylib"))
        from delugemu import DelugeSession
        from delugemu._controls import CONTROLS

        self.controls = CONTROLS
        self.dump = os.path.join(out, tag + ".s32")
        if os.path.exists(self.dump):
            os.unlink(self.dump)
        self.t = 0
        self.s = DelugeSession.launch(
            firmware=os.path.abspath(elf),
            sd=os.path.abspath(args.card),
            repo_root=args.delugemu,
            env={"DELUGEMU_QEMU_BIN": args.qemu, "DELUGEMU_SSIF_DUMP": self.dump},
            sock=f"/tmp/gen_emu_{os.getpid()}_{tag}.sock",
            run_log=os.path.join(out, tag + ".log"),
            extra_qemu=("-icount", "shift=2,sleep=off", "-S", "-snapshot"),
        )

    def close(self):
        self.s.close()

    def run_to(self, ns, each_step=None):
        while self.t < ns:
            self.t = min(ns, self.t + STEP)
            self.s.run_until(self.t, timeout=600)
            if each_step:
                each_step(self.t)

    def wait(self, ms):
        self.run_to(self.t + ms * MS)

    def button(self, name, down):
        c = self.controls[name]
        self.s.button(c["col"], c["row"], down)

    def tap(self, name, hold_ms=150, settle_ms=300):
        self.button(name, True)
        self.wait(hold_ms)
        self.button(name, False)
        self.wait(settle_ms)

    def shift_tap(self, name):
        self.button("SHIFT", True)
        self.wait(100)
        self.tap(name, settle_ms=100)
        self.button("SHIFT", False)
        self.wait(300)

    def led(self, name):
        return bool(self.s.read_leds().get(name))

    def oled(self):
        return hashlib.sha1(repr(self.s.read_oled()).encode()).hexdigest()

    def booted(self, limit_s=60):
        """Runs until the OLED is on and shows something, then 5 s more for the card and the boot song."""
        while not (self.s.oled_on() and any(any(row) for row in self.s.read_oled())):
            if self.t > limit_s * SEC:
                return False
            self.wait(100)
        self.wait(5000)
        return True

    def card_quiet(self, quiet_ms=1000, limit_s=60):
        """Runs until the firmware has issued no SD command for `quiet_ms` of virtual time. False if the card is
        still busy `limit_s` of virtual time after the call."""
        begin = self.t
        last, since = self.s.sd_command_count(), self.t
        while self.t - since < quiet_ms * MS:
            if self.t - begin >= limit_s * SEC:
                return False
            self.wait(100)
            count = self.s.sd_command_count()
            if count != last:
                last, since = count, self.t
        return True

    def dump_size(self):
        return os.path.getsize(self.dump) if os.path.exists(self.dump) else 0

    def audio(self, start, seconds):
        with open(self.dump, "rb") as fh:
            fh.seek(start)
            return fh.read(int(seconds * RATE) * FRAME)


def boot(args, elf, out, tag):
    """Launches and boots, relaunching a blank boot up to test_port_emu.BOOT_TRIES times."""
    for attempt in range(1, test_port_emu.BOOT_TRIES + 1):
        emu = Emu(args, elf, out, f"{tag}-boot{attempt}")
        if emu.booted():
            log(f"booted (attempt {attempt}) at {emu.t / SEC:.1f} s virtual")
            return emu
        log(f"boot {attempt} came up blank, retrying")
        emu.close()
    raise RuntimeError(f"no boot in {test_port_emu.BOOT_TRIES} tries")


def enter_perform(emu):
    """SHIFT+SONG to Song view, SHIFT+SONG again for GENERATIVE, then PERFORM FROM -> GENRE -> the first genre ->
    DISCARD SONG? -> OK (spec §14.1, §14.3). A capture build plays its own genre whatever row is picked."""
    emu.shift_tap("SESSION_VIEW")
    emu.wait(1500)  # the zoom out to Song view; SHIFT+SONG needs no UI mode active
    emu.shift_tap("SESSION_VIEW")
    for _ in range(4):  # PERFORM, GENRE, the genre, OK
        emu.tap("SELECT_ENC")
    return emu.card_quiet(limit_s=30)


def check_part_focus(emu, check):
    """Part focus on the SONG page (spec §13.5, §19): AFFECT ENTIRE is lit at the Recipe level, dark while a part's row
    is held, and SONG stays lit and CLIP dark over the stock LED updates that binding the knobs makes."""
    check("AFFECT ENTIRE lit at the Recipe level", emu.led("AFFECT_ENTIRE"))
    emu.s.pad_event(0, 7, True)  # row 1 (firmware y 7): the first part
    emu.wait(300)
    check(
        "holding a part row focuses it (AFFECT ENTIRE dark, SONG lit, CLIP dark)",
        not emu.led("AFFECT_ENTIRE")
        and emu.led("SESSION_VIEW")
        and not emu.led("CLIP_VIEW"),
    )
    emu.s.pad_event(0, 7, False)
    emu.wait(300)
    check("releasing it goes back to the Recipe level", emu.led("AFFECT_ENTIRE"))


def check_perform_plays(emu, seconds, check):
    """PLAY, then `seconds` of Perform: sound all the way and an OLED that keeps moving. Returns the rendered PCM."""
    check(
        "entered Perform on the SONG page (BACK and SONG lit, CLIP dark)",
        emu.led("BACK") and emu.led("SESSION_VIEW") and not emu.led("CLIP_VIEW"),
    )
    check("stopped on entry", not emu.led("PLAY"))
    check_part_focus(emu, check)
    start = emu.dump_size()
    emu.button("PLAY", True)
    emu.wait(150)
    emu.button("PLAY", False)
    check("PLAY starts Perform", emu.led("PLAY"))
    frames = []
    emu.run_to(
        emu.t + int(seconds * SEC), each_step=lambda t: frames.append((t, emu.oled()))
    )
    pcm = emu.audio(start, seconds)
    for failure in audio_failures(pcm, seconds):
        check(failure, False)
    check("Perform sounds all the way through", not audio_failures(pcm, seconds))
    for failure in freeze_failures(frames):
        check(failure, False)
    check("the OLED keeps moving", not freeze_failures(frames))
    return pcm


def write_wav(path, pcm):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(4)
        w.setframerate(RATE)
        w.writeframes(pcm)


# -- commands ----------------------------------------------------------------


def warn_pin(args):
    installed = installed_versions(args.qemu, args.delugemu)
    for warning in pin_warnings(read_pin(), installed):
        log("WARNING " + warning)
    return installed


def leak(args):
    """The leak check. Returns 0 if it passes."""
    elf = args.testport_elf or test_port_emu.build_testport()
    out = os.path.join(args.out, "leak")
    script = deluge_probe.read_script(os.path.join(HERE, "perform_mem_cycle.txt"))
    failed_expects = 0
    log(f"leak check: {args.cycles} Perform enter/leave cycles on {elf}")
    try:
        with test_port_emu.EmuRun(
            elf, out, args.delugemu, args.qemu, args.card, tag="leak"
        ) as emu:
            for n in range(1, args.cycles + 1):
                failed, total = deluge_probe.run_script(
                    emu.port,
                    script,
                    out=lambda line: None if line.startswith("PASS") else log(line),
                )
                log(f"leak cycle {n}: {total} expects, {failed} failed")
                failed_expects += failed
            events = list(emu.port.events)
    except (RuntimeError, TimeoutError) as exc:
        log(f"FAIL leak check: {exc}")
        return 1
    lines, failures = leak_report(mem_cycles(events), args.cycles)
    for line in lines:
        log("  " + line)
    if failed_expects:
        failures.append(f"{failed_expects} expects failed in the cycles")
    for failure in failures:
        log("FAIL " + failure)
    log(
        "leak check: {} (logs in {})".format(
            "PASS" if not failures else f"FAIL, {len(failures)} problems", out
        )
    )
    return 1 if failures else 0


def smoke(args):
    installed = warn_pin(args)
    os.makedirs(args.out, exist_ok=True)
    seconds = args.bars * SECONDS_PER_BAR
    failed = total = 0

    def check(name, ok):
        nonlocal failed, total
        failed += 0 if ok else 1
        total += 1
        log("{} {}".format("PASS" if ok else "FAIL", name))

    emu = boot(args, args.elf, args.out, "smoke")
    try:
        check("boots", True)
        check("Perform's song builds", enter_perform(emu))
        check_perform_plays(emu, seconds, check)
        emu.tap("PLAY")
        check("PLAY stops", not emu.led("PLAY"))
        emu.tap("BACK", hold_ms=700)  # held: LEAVE PERFORM? (spec §13.8)
        emu.tap("SELECT_ENC")  # LEAVE
        emu.card_quiet()
        check("BACK held, then LEAVE, leaves Perform", not emu.led("BACK"))
        emu.s.screenshot(os.path.join(args.out, "smoke-end.png"))
    finally:
        emu.close()
    log(f"{total} checks, {failed} failed; logs in {args.out}")
    leak_failed = 0 if args.no_leak else leak(args)
    log(
        "smoke: functional {}/{} passed; leak check {}".format(
            total - failed,
            total,
            "skipped" if args.no_leak else ("FAIL" if leak_failed else "PASS"),
        )
    )
    if failed == 0 and args.update_pin:
        write_pin(installed)
        log(
            "delugemu.pin now {} / {}: commit it".format(
                installed["bundle"], installed["checkout"][:7]
            )
        )
    return 1 if failed or leak_failed else 0


def capture(args):
    warn_pin(args)
    elf = build_capture(args)
    os.makedirs(args.out, exist_ok=True)
    emu = boot(args, elf, args.out, "capture")
    failures = []
    try:
        if not enter_perform(emu):
            raise RuntimeError("Perform's song never finished loading")
        pcm = check_perform_plays(
            emu, args.seconds, lambda what, ok: ok or failures.append(what)
        )
    finally:
        emu.close()
    wav = capture_path(args, args.out)
    write_wav(wav, pcm)
    for failure in failures:
        log("WARNING " + failure)
    log(f"{wav} (sha256 {hashlib.sha256(pcm).hexdigest()[:16]})")
    if not args.no_play:
        subprocess.run(["afplay", wav], check=False)
    return 0


def add_leak_args(p):
    p.add_argument(
        "--cycles",
        type=int,
        default=LEAK_CYCLES,
        help=f"leak check: Perform enter/leave cycles (at least {WARMUP_CYCLES + 2})",
    )
    p.add_argument(
        "--testport-elf",
        help="leak check: this ENABLE_SYSEX_TEST firmware instead of building build-testport/",
    )


def parser():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = p.add_subparsers(dest="command", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument(
        "--card", default=os.path.join(HOME, "source", "delugemu-sd", "deluge_sd.img")
    )
    common.add_argument("--delugemu", default=os.path.join(HOME, "source", "delugemu"))
    common.add_argument("--qemu", default=QEMU)

    s = sub.add_parser(
        "smoke", parents=[common], help="boot, enter Perform, play N bars"
    )
    s.add_argument(
        "--elf", default=os.path.join(REPO, "build", "Release", "deluge.elf")
    )
    s.add_argument(
        "--bars",
        type=int,
        default=8,
        help="at least this many bars of Perform (2 s each)",
    )
    s.add_argument("--out", default=os.path.join(REPO, "build", "delugemu-smoke"))
    s.add_argument(
        "--update-pin",
        action="store_true",
        help="record the installed delugemu once the checks pass",
    )
    s.add_argument("--no-leak", action="store_true", help="skip the leak check")
    add_leak_args(s)
    lk = sub.add_parser(
        "leak", parents=[common], help="the memory check over enter/leave cycles"
    )
    add_leak_args(lk)
    lk.add_argument("--out", default=os.path.join(REPO, "build", "delugemu-smoke"))

    c = sub.add_parser(
        "capture", parents=[common], help="build, capture and play a WAV of Perform"
    )
    c.add_argument("--genre", default="techno")
    c.add_argument("--seed", type=uint32, default=1)
    c.add_argument(
        "--entropy",
        type=uint32,
        default=0,
        help="what the genre's facts are rolled from",
    )
    for m in MACROS:
        c.add_argument(
            "--" + m, type=int, choices=range(51), metavar="0-50", help="OLED points"
        )
    c.add_argument("--key", help="C, C#, Db ... B")
    c.add_argument("--scale", help="minor, phrygian, harmonic-minor ...")
    c.add_argument("--tempo", type=bpm, help="BPM, 20 to 999")
    c.add_argument("--swing", type=int, choices=range(-49, 50), metavar="-49..49")
    c.add_argument(
        "--seconds", type=float, default=60.0, help="Perform time to capture"
    )
    c.add_argument("--no-play", action="store_true")
    c.add_argument("--build-dir", default=os.path.join(REPO, "build-capture"))
    c.add_argument("--out", default=os.path.join(REPO, "build-capture", "captures"))
    return p


def main():
    args = parser().parse_args()
    args.out = os.path.abspath(args.out)  # run.sh runs in the delugemu checkout
    if getattr(args, "cycles", LEAK_CYCLES) < WARMUP_CYCLES + 2:
        sys.exit(f"--cycles must be at least {WARMUP_CYCLES + 2}")
    command = {"smoke": smoke, "leak": leak, "capture": capture}[args.command]
    sys.exit(command(args))


if __name__ == "__main__":
    main()
