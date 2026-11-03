# Reimplement by default; port only with a recorded reason

The Deluge firmware is GPL-3.0-or-later. Several reference sources (PERF-1, Tuna outside `tuna_marbles*`, AMB-1 up to v0.9) are GPL-3.0-only, so porting their code would make the shipped firmware GPLv3-only as a whole. Separate files don't avoid that: the binary is still one combined work. We reimplement by default and take only ideas from every source, because the Deluge platform is different enough that our own code is likely to fit it better. In PERF-1's case, its small integer and table logic costs about as much to rewrite as to port. Facts: [Licence compatibility for porting](https://github.com/gismya/deluge-generative/issues/22). Decision: [Porting policy](https://github.com/gismya/deluge-generative/issues/23).

## Rules

- **Ideas are always open.** Using a source's behaviour, structure and musical intent is reimplementation. Copying code or data verbatim, or transliterating it closely, is porting. Our genre tables use PERF-1 as a reference, but their values are our own, re-derived and tuned by ear. Copying PERF-1's tables row for row counts as porting.
- **Porting from permissive or compatible sources** (Grids GPL-3.0-or-later; Marbles, stmlib and `tuna_marbles*` MIT; AMB-1 v1.1+ MPL-2.0) is allowed when there's a reason for it. The reason goes in `LICENSING.md`. Duties:
  - Keep the original headers.
  - Include the full MIT permission notice in each MIT file.
  - Keep MPL code in separate MPL files, never mixed into existing GPL files.
  - Add a dated "modified" notice to every ported file.
  - Never use "Mutable Instruments" or its module names in our own naming.
- **Porting GPL-3.0-only code** changes the firmware's licence. It needs a new ADR that supersedes this one, written before the code lands. We're open to that if a real reason turns up.
- **Provenance**: a root `LICENSING.md` lists every ported file (our file, source project and file, version or commit, licence, reason, changes) and the sources whose ideas we credit. It's created with the first ported or credited file. Upstream's existing third-party code isn't listed.
- **The Generative pack** is SD card content shipped separately from the firmware, so its licences don't affect the firmware. It carries its own licence and provenance list and may include GPL-3.0-only data such as PERF-1's DX7 voices. That covers the "DY" set too: we accept PERF-1's assessment of those voices as public domain.
