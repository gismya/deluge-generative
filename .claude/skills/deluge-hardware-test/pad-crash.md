# Decoding a crash on the pads

When the CPU faults, `handle_cpu_fault()` in `src/OSLikeStuff/fault_handler/fault_handler.c` draws up to four code
addresses on the main grid and the crashed build's commit in the sidebar, then the device stays frozen.
`FREEZE_WITH_ERROR` draws the same way and also shows its error code on the OLED. The device can't send anything
over USB by then, so a photo is the only way to read it.

## Steps

1. **Get a photo** of the whole grid and both sidebar columns, plus what the OLED shows. A frozen OLED with garbage
   on it is normal: the screen stops mid-frame.
2. **Transcribe it** into a text file: 8 lines, top row first, 18 characters each (main columns 0-15, then the two
   sidebar columns). Use `.` for unlit, and `m` `b` `g` `c` `r` `y` `w` for magenta, blue, green, cyan, red, yellow,
   white. Read pad by pad, and count each row's lit pads against the photo before moving on. One misplaced pad
   changes an address by a power of two. [example-crash.txt](example-crash.txt) is a real one.
3. **Find the ELF of the build that crashed.** The sidebar shows the first 4 hex digits of that build's commit. If
   `build/RelWithDebInfo/deluge.elf` has been rebuilt since, rebuild that commit in its own worktree. The commit is
   whatever HEAD was at build time, and uncommitted changes don't show in it. So a match proves the commit, not the
   code. Keep note of what was in the working tree when you load a build.
4. **Decode**:
   ```shell
   ./dbt exec "python3 scripts/generative/decode_pad_crash.py crash.txt --elf build/RelWithDebInfo/deluge.elf"
   ```
   It refuses an ELF from another commit. Pass `--any-commit` only when you know the code is the same, for example
   the same tree rebuilt after a commit.
5. **Read the result** with the layout below. The magenta and blue lines are facts. The green and cyan lines are
   hints. A fault in code that touches memory (a `ldr` or `str` at the PC) usually means a bad pointer or a
   corrupted global. Look at what the instruction indexes, and at what sits next to that data in `nm -n`.

## The layout

- **Bits.** Each column is one byte: the bottom pad is bit 7, the top pad bit 0. Four columns make one 32-bit
  address, most significant byte on the left.
- **Groups of 4 columns, by colour:**
  - **magenta**: the faulting instruction (the exception PC). Drawn only when it points into code, so a crash
    from a null or garbage function pointer has none.
  - **blue**: the caller, SYS-mode LR − 2, which lands inside the call instruction.
  - **green, cyan** (alternating): code addresses found by scanning the stack upward, each − 2. They are
    hints: any stale word that looks like code qualifies. A value 2 below a function's start was a function
    pointer on the stack, not a return address. The script says so when it sees one.
  - Unused groups are dark.
- **Sidebar.** The left column is the commit's first byte and the right column the second. The colour is the fault
  type. `get_colours()` is passed to `drawByte()` with green and blue swapped, so read it as: **red** data abort,
  **blue** undefined instruction, **green** prefetch abort, **yellow** reserved exception, **magenta** soft fault
  (`FREEZE_WITH_ERROR`), **cyan** SVC.
- **Thumb.** Code addresses are Thumb: an odd value means the instruction at the even address just below it. The
  script masks bit 0 before looking anything up.
- **Inlining.** LTO inlines heavily, so the `nm` symbol is often a big caller. The script's `source:` lines come
  from `addr2line -i` and name the inlined function, with a file and line when the build kept one.
