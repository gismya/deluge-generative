# Decoding a crash on the pads

When the CPU faults, `handle_cpu_fault()` in `src/OSLikeStuff/fault_handler/fault_handler.c` draws up to four code
addresses on the main grid and the crashed build's commit in the sidebar, then the device stays frozen.
`FREEZE_WITH_ERROR` draws the same way, without the faulting instruction, and shows its error code on the OLED. It
waits for a press of the select knob, which resumes and lets the pads redraw. The device can't send anything over
USB by then, so a photo is the only way to read it.

## Steps

1. **Get a photo** of the whole grid and both sidebar columns, plus what the OLED shows. A frozen OLED with garbage
   on it is normal: the screen stops mid-frame. If the OLED shows an error code, ask the user not to press the
   select knob until the photo is taken.
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
5. **Read the result** with the layout below. The magenta line is a fact. The blue line is the last call that ran:
   check that its `bl` target is the magenta function before calling it the caller. The green and cyan lines are
   hints. A fault in code that touches memory (a `ldr` or `str` at the PC) usually means a bad pointer or a
   corrupted global. Look at what the instruction indexes, and at what sits next to that data in `nm -n`.

## The layout

- **Bits.** Each column is one byte: the bottom pad is bit 7, the top pad bit 0. Four columns make one 32-bit
  address, most significant byte on the left.
- **Groups of 4 columns, by colour:**
  - **magenta**: the faulting instruction (the exception PC). Drawn only when it points into code, so a crash
    from a null or garbage function pointer has none. For an undefined instruction or SVC in Thumb code it's 2
    bytes early: the fault entry subtracts 4, but the CPU saved only 2 past it. The script adds the 2 back.
  - **blue**: SYS-mode LR − 2, which lands inside the last call that ran. If the faulting function had made no
    call yet, that's the call into it. Otherwise it's the last call the function made, or a call inside that one.
    For `FREEZE_WITH_ERROR` it's the LR at the freeze.
  - **green, cyan** (alternating, green first): code addresses found by scanning the stack upward, each − 2.
    They are hints: any stale word that looks like code qualifies. A value 2 below a function's start was usually
    a function pointer on the stack, not a return address. It can also be the return of a call that ends the
    function before it. The script tells the two apart by the instruction there.
  - Unused groups are dark only when the stack scan found nothing. Once it finds one address, every remaining
    group is drawn, and slots with nothing found show `0xFFFFFFFE` (every pad lit but the top one of the group's
    last column). The script marks them unused.
- **Sidebar.** The left column is the commit's first byte and the right column the second. The colour is the fault
  type. `get_colours()` is passed to `drawByte()` with green and blue swapped, so read it as: **red** data abort,
  **blue** undefined instruction, **green** prefetch abort, **yellow** reserved exception, **magenta** soft fault
  (`FREEZE_WITH_ERROR`), **cyan** SVC.
- **Thumb.** Code addresses are Thumb: an odd value means the instruction at the even address just below it. The
  script masks bit 0 before looking anything up.
- **Inlining.** LTO inlines heavily, so the `nm` symbol is often a big caller. The script's `source:` lines come
  from `addr2line -i` and name the inlined function, with a file and line when the build kept one.
