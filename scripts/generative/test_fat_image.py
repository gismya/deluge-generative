"""Tests for fat_image.py on small FAT32 images the tests format themselves. Where the host has fsck_msdos (macOS)
or fsck.fat (dosfstools), each edited image is also checked by it.

python3 scripts/generative/test_fat_image.py
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fat_image

CLUSTERS = 66000  # FAT32 needs at least 65525
FSCK = next(
    (
        [path, "-n"]
        for path in ("/sbin/fsck_msdos", shutil.which("fsck.fat") or "")
        if path and os.path.exists(path)
    ),
    None,
)


def make_fat32(path, clusters=CLUSTERS):
    """A blank whole-image FAT32 (as newfs_msdos makes delugemu's card) with one 512-byte sector a cluster."""
    bps, reserved, fats = 512, 32, 2
    fat_sectors = -(-(clusters + 2) * 4 // bps)
    total = reserved + fats * fat_sectors + clusters
    boot = bytearray(bps)
    boot[0:3] = b"\xeb\x58\x90"
    boot[3:11] = b"BSD  4.4"
    struct.pack_into(
        "<HBHBHHBHHHII",
        boot,
        11,
        bps,
        1,
        reserved,
        fats,
        0,
        0,
        0xF8,
        0,
        32,
        64,
        0,
        total,
    )
    struct.pack_into("<IHHIHH", boot, 36, fat_sectors, 0, 0, 2, 1, 6)
    struct.pack_into(
        "<BBBI11s8s", boot, 64, 0x80, 0, 0x29, 1234, b"NO NAME    ", b"FAT32   "
    )
    boot[510:512] = b"\x55\xaa"
    info = bytearray(bps)
    info[0:4], info[484:488] = b"RRaA", b"rrAa"
    struct.pack_into("<II", info, 488, clusters - 1, 3)
    info[508:512] = b"\x00\x00\x55\xaa"
    fat = struct.pack("<III", 0x0FFFFFF8, 0x0FFFFFFF, 0x0FFFFFFF)
    with open(path, "wb") as fh:
        fh.truncate(total * bps)
        for sector, data in ((0, boot), (1, info), (6, boot), (7, info)):
            fh.seek(sector * bps)
            fh.write(data)
        for i in range(fats):
            fh.seek((reserved + i * fat_sectors) * bps)
            fh.write(fat)


def free_clusters(path):
    with fat_image.FatImage(path) as card:
        return sum(1 for v in card.fat[2:] if v & 0x0FFFFFFF == 0)


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = tmp.name
        self.img = os.path.join(self.tmp, "card.img")
        make_fat32(self.img)

    def open(self):
        return fat_image.FatImage(self.img)

    def assert_fsck_clean(self):
        if FSCK is None:
            return
        proc = subprocess.run(
            [*FSCK, self.img], capture_output=True, text=True, check=False
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)


class WriteAndRead(Base):
    def test_a_long_name_in_new_folders_reads_back_after_reopening(self):
        data = b"<palette/>" * 200  # four clusters
        with self.open() as card:
            card.write_file("GENERATIVE/PALETTES/ACID TECHNO.XML", data)
        with self.open() as card:
            self.assertEqual(card.listdir(), ["GENERATIVE"])
            self.assertEqual(card.listdir("GENERATIVE/PALETTES"), ["ACID TECHNO.XML"])
            self.assertEqual(
                card.read_file("generative/palettes/acid techno.xml"), data
            )
        self.assert_fsck_clean()

    def test_an_empty_file_has_no_clusters(self):
        with self.open() as card:
            card.write_file("EMPTY.TXT", b"")
        with self.open() as card:
            self.assertEqual(card.read_file("EMPTY.TXT"), b"")
        self.assertEqual(free_clusters(self.img), CLUSTERS - 1)
        self.assert_fsck_clean()

    def test_names_that_share_a_short_basis_get_their_own_short_names(self):
        with self.open() as card:
            for i in range(3):
                card.write_file(f"SAMPLES/Mine kick {i}.wav", bytes([i]) * 700)
        with self.open() as card:
            self.assertEqual(
                card.listdir("SAMPLES"), [f"Mine kick {i}.wav" for i in range(3)]
            )
            for i in range(3):
                self.assertEqual(
                    card.read_file(f"SAMPLES/Mine kick {i}.wav"), bytes([i]) * 700
                )
        self.assert_fsck_clean()

    def test_a_folder_grows_past_one_cluster(self):
        names = [f"A rather long file name {i:02}.XML" for i in range(40)]
        with self.open() as card:
            for name in names:
                card.write_file(f"SYNTHS/{name}", name.encode())
        with self.open() as card:
            self.assertEqual(card.listdir("SYNTHS"), names)
            self.assertEqual(card.read_file(f"SYNTHS/{names[-1]}"), names[-1].encode())
        self.assert_fsck_clean()

    def test_the_free_count_in_fsinfo_is_kept(self):
        with self.open() as card:
            card.write_file("A/B.BIN", bytes(1500))
        with open(self.img, "rb") as fh:
            fh.seek(512 + 488)
            free = struct.unpack("<I", fh.read(4))[0]
        self.assertEqual(free, free_clusters(self.img))

    def test_fsinfos_next_free_hint_wraps_after_the_last_cluster(self):
        with self.open() as card:
            card.next_free = card.clusters + 1  # the last cluster
            card.write_file("LAST.BIN", bytes(512))
            self.assertEqual(
                card.chain(card._lookup("LAST.BIN").cluster), [card.clusters + 1]
            )
        with open(self.img, "rb") as fh:
            fh.seek(512 + 492)
            hint = struct.unpack("<I", fh.read(4))[0]
        self.assertEqual(hint, 2)
        self.assert_fsck_clean()


class Replace(Base):
    def test_a_file_written_again_is_replaced_and_its_old_clusters_freed(self):
        with self.open() as card:
            card.write_file("KICK.WAV", b"a" * 2000)
            before = sum(1 for v in card.fat[2:] if v & 0x0FFFFFFF == 0)
            card.write_file("kick.wav", b"b" * 2000)
            after = sum(1 for v in card.fat[2:] if v & 0x0FFFFFFF == 0)
        self.assertEqual(before, after)
        with self.open() as card:
            self.assertEqual(card.listdir(), ["kick.wav"])
            self.assertEqual(card.read_file("KICK.WAV"), b"b" * 2000)
        self.assert_fsck_clean()


class Remove(Base):
    def test_a_folder_goes_with_everything_in_it(self):
        with self.open() as card:
            card.write_file("KEEP.TXT", b"keep")
            for i in range(20):
                card.write_file(f"GENERATIVE/SAMPLES/Pack file {i}.wav", bytes(600))
        with self.open() as card:
            card.remove("GENERATIVE")
        with self.open() as card:
            self.assertEqual(card.listdir(), ["KEEP.TXT"])
            self.assertFalse(card.exists("GENERATIVE/SAMPLES"))
        self.assertEqual(free_clusters(self.img), CLUSTERS - 2)
        self.assert_fsck_clean()

    def test_a_removed_files_slots_are_used_again(self):
        with self.open() as card:
            card.write_file("SYNTHS/165 Acid Bass.XML", b"x")
            card.remove("SYNTHS/165 Acid Bass.XML")
            card.write_file("SYNTHS/015 Acid Bass.XML", b"y")
        with self.open() as card:
            self.assertEqual(card.listdir("SYNTHS"), ["015 Acid Bass.XML"])
        self.assert_fsck_clean()

    def test_a_missing_path_is_an_error(self):
        with self.open() as card, self.assertRaises(FileNotFoundError):
            card.remove("NOPE")


class Refuses(Base):
    def test_an_image_with_a_partition_table(self):
        with open(self.img, "r+b") as fh:
            fh.write(bytes(510))
        with self.assertRaisesRegex(ValueError, "partition table"):
            self.open()

    def test_fat16(self):
        with open(self.img, "r+b") as fh:
            fh.seek(22)
            fh.write(struct.pack("<H", 200))
        with self.assertRaisesRegex(ValueError, "not FAT32"):
            self.open()


class CopyTree(Base):
    def test_copies_a_host_folder_and_skips_dot_files(self):
        src = os.path.join(self.tmp, "overlay")
        os.makedirs(os.path.join(src, "GENERATIVE", "PALETTES"))
        for name, data in (
            ("GENERATIVE/PALETTES/ACID TECHNO.XML", b"acid"),
            ("GENERATIVE/PALETTES/._ACID TECHNO.XML", b"appledouble"),
            (".DS_Store", b"finder"),
        ):
            with open(os.path.join(src, *name.split("/")), "wb") as fh:
                fh.write(data)
        with self.open() as card:
            written = fat_image.copy_tree(card, src)
        self.assertEqual(written, ["GENERATIVE/PALETTES/ACID TECHNO.XML"])
        with self.open() as card:
            self.assertEqual(card.listdir("GENERATIVE/PALETTES"), ["ACID TECHNO.XML"])
            self.assertEqual(card.listdir(), ["GENERATIVE"])


if __name__ == "__main__":
    unittest.main()
