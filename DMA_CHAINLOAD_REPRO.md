# TEST ONLY: reproducing the chainload DMA crash

Not for merging. These two commits on upstream `main` are the builds used to verify
[SynthstromAudible/DelugeFirmware#5371](https://github.com/SynthstromAudible/DelugeFirmware/pull/5371) (stop every DMA
channel before a chainload).

## The builds

- **L** (the first commit): `resetprg()` records every DMA channel's CHSTAT and CRLA before anything else, and the OLED
  shows the channels that were running as `BOOTDMA <mask>`, with the first one's last link descriptor address.
- **T** (the second commit, this branch's head): L plus a 48 KB `.dma_poison` block placed first in `.rodata`. It holds
  1 KB of Thumb `UDF` words, then 1504 copies of a link descriptor that copies that 1 KB over the start of `resetprg()`.
  They cover the addresses where L keeps its real descriptors (SSI TX/RX, PIC RX, MIDI timing), 32-byte aligned. Nothing
  in T reads the block, so it only matters if a DMA channel is still running after the chainload.

Build both as Release with `-DENABLE_SYSEX_LOAD=ON`. Check that T covers L's descriptors: with `nm -n` on L's ELF, each
`*LinkDescriptor*` address must lie between `dmaPoison + 0x400` and `dmaPoison + 0xC000` in T's ELF.

T's poison writes to a fixed address, `__dma_poison_target` in `linker_script_rz_a1l.ld`. If `resetprg()` moves, the link
fails with a message saying so: set the number to `resetprg`'s address from `nm`, without the Thumb bit.

## Results on an OLED Deluge

| Loader | Loaded | Result |
|---|---|---|
| L (no fix) | L | Boots, `BOOTDMA 70C0`: channels 6, 7, 12, 13 and 14 still running, on L's descriptors |
| `main` + #5371 | L | Boots, `BOOTDMA 0000` |
| L (no fix), flashed from SD | T | Crashes: undefined instruction at `resetprg+0` |
| `main` + #5371 | T | Boots normally, `BOOTDMA 0000` |
