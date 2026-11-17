"""Edits files on a FAT32 card image in place, with no mount: no sudo, no GUI, and no macOS `._*` or Spotlight files
on the card. For delugemu's card (test_port_emu.py --card-overlay, #277).

    with fat_image.FatImage("card.img") as card:
        card.write_file("GENERATIVE/PALETTES/ACID TECHNO.XML", data)   # makes the folders, replaces a file
        card.remove("SYNTHS/165 Acid Bass.XML")                           # a file, or a folder and all in it
        card.listdir("SYNTHS"), card.read_file(path), card.exists(path)

Only FAT32 on the whole image (a "superfloppy", as delugemu's card and newfs_msdos make it): an image with a partition
table, or FAT12/16, is refused. Names are matched without case, as FAT does; a new name gets long-name entries
unless it is a plain upper-case 8.3 name. The FAT copies and FSInfo's free count are written on close.

Stdlib only.
"""

import array
import itertools
import os
import struct
import sys

FREE, BAD, END = 0, 0x0FFFFFF7, 0x0FFFFFFF
ENTRY = 32
ATTR_DIR, ATTR_ARCHIVE, ATTR_LFN, ATTR_VOLUME = 0x10, 0x20, 0x0F, 0x08
DELETED = 0xE5
LFN_CHARS = 13  # UTF-16 units in one long-name entry
SHORT_CHARS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$%'-_@~`!(){}^#&")
# 2026-01-01 00:00, so a card built twice is the same
FAT_DATE, FAT_TIME = ((2026 - 1980) << 9) | (1 << 5) | 1, 0


def _short_checksum(short):
    total = 0
    for byte in short:
        total = (((total & 1) << 7) + (total >> 1) + byte) & 0xFF
    return total


def _plain_short(name):
    """The 11-byte short name for `name` if it is already a plain upper-case 8.3 name, else None."""
    if name in (".", ".."):
        return None
    base, dot, ext = name.partition(".")
    if (
        not base
        or len(base) > 8
        or len(ext) > 3
        or (dot and not ext)
        or "." in ext
        or not set(base + ext) <= SHORT_CHARS
    ):
        return None
    return (base.ljust(8) + ext.ljust(3)).encode("ascii")


def _short_basis(name):
    """The basis of a generated short name (FAT's rules, simplified): upper case, other characters as "_", no
    spaces or dots but the last."""
    stem, dot, ext = name.strip(". ").rpartition(".")
    if not dot:
        stem, ext = ext, ""

    def clean(text):
        text = text.upper().replace(" ", "").replace(".", "")
        return "".join(c if c in SHORT_CHARS else "_" for c in text)

    return clean(stem) or "_", clean(ext)[:3]


class Entry:
    """A directory entry: its name (the long one if it has one), short name, attributes, first cluster, size, and
    where its slots are (the long-name ones, then the short one) as (cluster, offset) pairs."""

    def __init__(self, name, short, attr, cluster, size, slots):
        self.name, self.short, self.attr = name, short, attr
        self.cluster, self.size, self.slots = cluster, size, slots

    @property
    def is_dir(self):
        return bool(self.attr & ATTR_DIR)


class FatImage:
    def __init__(self, path):
        self.fh = open(path, "r+b")  # noqa: SIM115 - closed by close()
        try:
            self._read_boot()
        except Exception:
            self.fh.close()
            raise
        self.dirty = False

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # The layout -----------------------------------------------------------

    def _read_boot(self):
        boot = self.fh.read(512)
        if len(boot) < 512 or boot[510:512] != b"\x55\xaa":
            raise ValueError("not a FAT image: no boot sector signature")
        (bps, spc, reserved, fats, root_entries, total16, _media, fat16) = (
            struct.unpack_from("<HBHBHHBH", boot, 11)
        )
        total32, fat32, flags, _version, root, fsinfo = struct.unpack_from(
            "<IIHHIH", boot, 32
        )
        if bps == 0 or spc == 0 or fats == 0:
            raise ValueError(
                "not a FAT image on its own (a partition table?): only a whole-image FAT32 is supported"
            )
        if fat16 != 0 or root_entries != 0 or fat32 == 0:
            raise ValueError("not FAT32: only FAT32 is supported")
        self.bps, self.cluster_size = bps, bps * spc
        self.fat_offset, self.fat_bytes, self.fats = reserved * bps, fat32 * bps, fats
        self.mirrored = not flags & 0x80
        self.active_fat = 0 if self.mirrored else flags & 0x0F
        self.data_offset = (reserved + fats * fat32) * bps
        total = total32 or total16
        self.clusters = (total - reserved - fats * fat32) // spc
        self.root = root
        self.fsinfo_offset = fsinfo * bps if fsinfo not in (0, 0xFFFF) else None
        self.fh.seek(self.fat_offset + self.active_fat * self.fat_bytes)
        self.fat = array.array("I")
        self.fat.frombytes(self.fh.read((self.clusters + 2) * 4))
        if sys.byteorder == "big":
            self.fat.byteswap()
        self.next_free = 2

    def close(self):
        if self.fh.closed:
            return
        try:
            if self.dirty:
                self._write_fat()
        finally:
            self.fh.close()

    def _write_fat(self):
        fat = array.array("I", self.fat)
        if sys.byteorder == "big":
            fat.byteswap()
        data = fat.tobytes()
        for i in range(self.fats) if self.mirrored else [self.active_fat]:
            self.fh.seek(self.fat_offset + i * self.fat_bytes)
            self.fh.write(data)
        if self.fsinfo_offset is not None:
            self.fh.seek(self.fsinfo_offset)
            info = self.fh.read(512)
            if info[0:4] == b"RRaA" and info[484:488] == b"rrAa":
                free = sum(1 for v in self.fat[2:] if v & 0x0FFFFFFF == FREE)
                self.fh.seek(self.fsinfo_offset + 488)
                self.fh.write(struct.pack("<II", free, self.next_free))
        self.dirty = False

    # Clusters -------------------------------------------------------------

    def _get(self, cluster):
        return self.fat[cluster] & 0x0FFFFFFF

    def _set(self, cluster, value):
        self.fat[cluster] = (self.fat[cluster] & 0xF0000000) | value
        self.dirty = True

    def chain(self, cluster):
        clusters = []
        while 2 <= cluster < BAD and len(clusters) <= self.clusters:
            clusters.append(cluster)
            cluster = self._get(cluster)
        return clusters

    def _allocate(self, count):
        """`count` free clusters, chained and ended. Raises OSError when the card is full."""
        found = []
        cluster = self.next_free
        for _ in range(self.clusters):
            if not 2 <= cluster < self.clusters + 2:
                cluster = 2
            if self._get(cluster) == FREE:
                found.append(cluster)
                if len(found) == count:
                    break
            cluster += 1
        if len(found) < count:
            raise OSError("the card image is full")
        for a, b in itertools.pairwise(found):
            self._set(a, b)
        self._set(found[-1], END)
        self.next_free = found[-1] + 1
        return found

    def _free(self, cluster):
        for c in self.chain(cluster):
            self._set(c, FREE)

    def _offset(self, cluster):
        return self.data_offset + (cluster - 2) * self.cluster_size

    def _read_cluster(self, cluster):
        self.fh.seek(self._offset(cluster))
        return self.fh.read(self.cluster_size)

    def _write_at(self, cluster, offset, data):
        self.fh.seek(self._offset(cluster) + offset)
        self.fh.write(data)

    # Directories ----------------------------------------------------------

    def _entries(self, cluster):
        """The live entries of the directory at `cluster`, "." and ".." left out."""
        entries, lfn, lfn_slots = [], {}, []
        for c in self.chain(cluster):
            data = self._read_cluster(c)
            for offset in range(0, self.cluster_size, ENTRY):
                raw = data[offset : offset + ENTRY]
                if raw[0] == 0:
                    return entries
                if raw[0] == DELETED:
                    lfn, lfn_slots = {}, []
                    continue
                attr = raw[11]
                if attr & 0x3F == ATTR_LFN:
                    if raw[0] & 0x40:
                        lfn, lfn_slots = {}, []
                    part = raw[1:11] + raw[14:26] + raw[28:32]
                    lfn[raw[0] & 0x1F] = (part, raw[13])
                    lfn_slots.append((c, offset))
                    continue
                short = raw[:11]
                if attr & ATTR_VOLUME or short in (b".          ", b"..         "):
                    lfn, lfn_slots = {}, []
                    continue
                name = self._long_name(lfn, short) or self._short_text(raw)
                first = (
                    struct.unpack_from("<H", raw, 20)[0] << 16
                    | struct.unpack_from("<H", raw, 26)[0]
                )
                size = struct.unpack_from("<I", raw, 28)[0]
                entries.append(
                    Entry(name, short, attr, first, size, [*lfn_slots, (c, offset)])
                )
                lfn, lfn_slots = {}, []
        return entries

    @staticmethod
    def _long_name(lfn, short):
        if not lfn or sorted(lfn) != list(range(1, len(lfn) + 1)):
            return None
        checksum = _short_checksum(short)
        if any(check != checksum for _, check in lfn.values()):
            return None
        units = b"".join(lfn[i][0] for i in sorted(lfn)).decode("utf-16-le")
        return units.split("\x00", 1)[0]

    @staticmethod
    def _short_text(raw):
        base, ext = (
            raw[:8].decode("latin-1").rstrip(),
            raw[8:11].decode("latin-1").rstrip(),
        )
        if raw[0] == 0x05:
            base = "\xe5" + base[1:]
        if raw[12] & 0x08:
            base = base.lower()
        if raw[12] & 0x10:
            ext = ext.lower()
        return base + ("." + ext if ext else "")

    @staticmethod
    def _parts(path):
        return [p for p in path.replace("\\", "/").split("/") if p]

    def _find(self, cluster, name):
        folded = name.casefold()
        return next(
            (e for e in self._entries(cluster) if e.name.casefold() == folded), None
        )

    def _lookup(self, path):
        """The entry at `path`, or None. The root has no entry: "" gives a stand-in for it."""
        entry = Entry("", b"", ATTR_DIR, self.root, 0, [])
        for part in self._parts(path):
            if not entry.is_dir:
                return None
            entry = self._find(entry.cluster, part)
            if entry is None:
                return None
        return entry

    def _dir_cluster(self, path, create=False):
        cluster = self.root
        for part in self._parts(path):
            entry = self._find(cluster, part)
            if entry is None and create:
                entry = self._make_dir(cluster, part)
            if entry is None or not entry.is_dir:
                raise FileNotFoundError(f"no folder {path!r} on the card")
            cluster = entry.cluster
        return cluster

    def _short_name(self, cluster, name):
        plain = _plain_short(name)
        taken = {e.short for e in self._entries(cluster)}
        if plain is not None and plain not in taken:
            return plain, False
        stem, ext = _short_basis(name)
        for n in range(1, 1000000):
            tail = f"~{n}"
            short = (stem[: 8 - len(tail)] + tail).ljust(8) + ext.ljust(3)
            short = short.encode("ascii")
            if short not in taken:
                return short, True
        raise OSError(f"no free short name for {name!r}")

    def _free_slots(self, cluster, count):
        """`count` free slots in a row in the directory at `cluster`, as (cluster, offset) pairs: deleted ones, or
        the unused ones after the end mark; the directory grows by a cleared cluster when it has none."""
        run = []
        clusters = self.chain(cluster)
        for c in clusters:
            data = self._read_cluster(c)
            for offset in range(0, self.cluster_size, ENTRY):
                if data[offset] in (0, DELETED):
                    run.append((c, offset))
                    if len(run) == count:
                        return run
                else:
                    run = []
        while len(run) < count:
            new = self._allocate(1)[0]
            self._set(clusters[-1], new)
            clusters.append(new)
            self._write_at(new, 0, bytes(self.cluster_size))
            run += [(new, offset) for offset in range(0, self.cluster_size, ENTRY)][
                : count - len(run)
            ]
        return run

    def _add_entry(self, cluster, name, attr, first, size):
        if not name or name in (".", "..") or any(c in name for c in '"*/:<>?\\|'):
            raise ValueError(f"not a FAT name: {name!r}")
        short, needs_long = self._short_name(cluster, name)
        units = name.encode("utf-16-le")
        if len(units) > 255 * 2:
            raise ValueError(f"name too long for FAT: {name!r}")
        entries = []
        if needs_long:
            checksum = _short_checksum(short)
            chunks = -(-len(units) // (LFN_CHARS * 2))
            padded = units + (b"\x00\x00" if len(units) % (LFN_CHARS * 2) else b"")
            padded = padded.ljust(chunks * LFN_CHARS * 2, b"\xff")
            for i in range(chunks, 0, -1):
                part = padded[(i - 1) * LFN_CHARS * 2 : i * LFN_CHARS * 2]
                order = i | (0x40 if i == chunks else 0)
                entries.append(
                    bytes([order])
                    + part[0:10]
                    + bytes([ATTR_LFN, 0, checksum])
                    + part[10:22]
                    + b"\x00\x00"
                    + part[22:26]
                )
        entries.append(self._short_entry(short, attr, first, size))
        for (c, offset), raw in zip(self._free_slots(cluster, len(entries)), entries):
            self._write_at(c, offset, raw)

    @staticmethod
    def _short_entry(short, attr, first, size):
        return struct.pack(
            "<11sBBBHHHHHHHI",
            short,
            attr,
            0,
            0,
            FAT_TIME,
            FAT_DATE,
            FAT_DATE,
            first >> 16,
            FAT_TIME,
            FAT_DATE,
            first & 0xFFFF,
            size,
        )

    def _make_dir(self, parent, name):
        cluster = self._allocate(1)[0]
        data = bytearray(self.cluster_size)
        data[0:ENTRY] = self._short_entry(b".          ", ATTR_DIR, cluster, 0)
        up = 0 if parent == self.root else parent
        data[ENTRY : 2 * ENTRY] = self._short_entry(b"..         ", ATTR_DIR, up, 0)
        self._write_at(cluster, 0, bytes(data))
        self._add_entry(parent, name, ATTR_DIR, cluster, 0)
        return self._find(parent, name)

    def _drop_entry(self, entry):
        for c, offset in entry.slots:
            self._write_at(c, offset, bytes([DELETED]))
        if entry.cluster:
            self._free(entry.cluster)

    # Files ----------------------------------------------------------------

    def exists(self, path):
        return self._lookup(path) is not None

    def listdir(self, path=""):
        """The names in the folder at `path`, in the directory's order."""
        return [e.name for e in self._entries(self._dir_cluster(path))]

    def is_dir(self, path):
        entry = self._lookup(path)
        return entry is not None and entry.is_dir

    def read_file(self, path):
        entry = self._lookup(path)
        if entry is None or entry.is_dir:
            raise FileNotFoundError(f"no file {path!r} on the card")
        data = b"".join(self._read_cluster(c) for c in self.chain(entry.cluster))
        return data[: entry.size]

    def write_file(self, path, data):
        """Writes `data` as the file at `path`, making its folders and replacing a file there."""
        parts = self._parts(path)
        if not parts:
            raise ValueError("no file name")
        parent = self._dir_cluster("/".join(parts[:-1]), create=True)
        old = self._find(parent, parts[-1])
        if old is not None:
            if old.is_dir:
                raise IsADirectoryError(f"{path!r} is a folder on the card")
            self._drop_entry(old)
        clusters = self._allocate(-(-len(data) // self.cluster_size)) if data else []
        for i, c in enumerate(clusters):
            chunk = data[i * self.cluster_size : (i + 1) * self.cluster_size]
            self._write_at(c, 0, chunk.ljust(self.cluster_size, b"\x00"))
        self._add_entry(
            parent, parts[-1], ATTR_ARCHIVE, clusters[0] if clusters else 0, len(data)
        )

    def makedirs(self, path):
        self._dir_cluster(path, create=True)

    def remove(self, path):
        """Removes the file at `path`, or the folder and everything in it."""
        entry = self._lookup(path)
        if entry is None or not self._parts(path):
            raise FileNotFoundError(f"no {path!r} on the card")
        if entry.is_dir:
            for child in self._entries(entry.cluster):
                self.remove(f"{path}/{child.name}")
        self._drop_entry(entry)

    def walk_files(self, path=""):
        """Every file under the folder at `path`, as card paths."""
        for entry in self._entries(self._dir_cluster(path)):
            child = f"{path}/{entry.name}" if path else entry.name
            if entry.is_dir:
                yield from self.walk_files(child)
            else:
                yield child


def copy_tree(card, source, target="", skip=()):
    """Copies every file under the host folder `source` onto `card` under `target`, making folders. Skips names that
    start with a dot (macOS's .DS_Store and ._* files), and the names in `skip` at the top of `source`. Returns the
    card paths written."""
    written = []
    for folder, dirs, files in os.walk(source):
        top = folder == source
        dirs[:] = sorted(
            d for d in dirs if not d.startswith(".") and not (top and d in skip)
        )
        files = [f for f in files if not (top and f in skip)]
        rel = os.path.relpath(folder, source)
        base = "/".join(p for p in [target, "" if rel == "." else rel] if p)
        base = base.replace(os.sep, "/")
        for name in sorted(files):
            if name.startswith("."):
                continue
            with open(os.path.join(folder, name), "rb") as fh:
                data = fh.read()
            path = f"{base}/{name}" if base else name
            card.write_file(path, data)
            written.append(path)
    return written
