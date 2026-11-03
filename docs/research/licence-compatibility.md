# Licence compatibility for porting

Research for [#22](https://github.com/gismya/deluge-generative/issues/22), part of the map [#1](https://github.com/gismya/deluge-generative/issues/1). Researched 2026-11-03.

**This is not legal advice.** It reads the licence texts and the projects' own licensing statements. It does not settle contested questions of law, such as whether a synth patch can be copyrighted. If something here matters commercially, ask a lawyer.

## Verdicts

| Source | Its licence | Verdict for this firmware | Duties if code is ported |
| --- | --- | --- | --- |
| Deluge community firmware (this tree) | GPL-3.0-or-later (file headers) | n/a | n/a |
| PERF-1 | GPL-3.0-only | **May port code** | Keep the copyright and SPDX lines. Mark the files as modified. The combined firmware can then only be distributed under GPLv3, not a later GPL. |
| AMB-1 after 0.9 (v1.1 and later) | MPL-2.0, no Exhibit B | **May port code** | Ported files stay MPL-2.0: keep their notices and offer their source. The firmware as a whole stays GPL. |
| AMB-1 0.9 and earlier | GPL-3.0-only | **May port code** | Same as PERF-1. |
| Mutable Instruments Grids | GPL-3.0-or-later | **May port code** (pattern tables included) | Keep the GPL header and copyright, and mark the files as modified. Don't use "Mutable Instruments" or "Grids" as the feature's name. |
| Mutable Instruments Marbles (and stmlib) | MIT | **May port code** | Put the full MIT copyright and permission notice in every ported file. Same naming rule as Grids. |
| Tuna, its own `tuna_*` code | GPL-3.0-only | **May port code** | Same as PERF-1. |
| Tuna `tuna_marbles*` | MIT (a port of Marbles) | **May port code** | Keep Emilie Gillet's MIT notice. Credit Tuna's port as well. |
| PERF-1's "DY" DX7 voices | "Public domain" on the word of a third-party site | **Don't bundle.** Users can load them as `.syx` files. | Not applicable |
| PERF-1's "DX" factory voices (Melodee) | GPL-3.0-only | May port | Same as PERF-1 code. |

No source on the list falls under "ideas only" or "not at all". Every licence found is GPLv3-compatible. Two things set the real limits. Porting any GPL-3.0-only code makes the shipped firmware GPLv3-only. And the DY voice data has weak provenance.

## 1. What licence this firmware is under

The tree is **GPL-3.0-or-later**. Evidence, on `generative` at `3ca05672` (upstream `v1.3.0-rc3` plus one commit):

- `LICENSE` is the plain GPLv3 text. Read alone, it doesn't decide between "only" and "or later". GitHub's API reports upstream SynthstromAudible/DelugeFirmware as `GPL-3.0`, a value that likewise covers both.
- The file headers decide it. 955 files under `src/` say "either version 3 of the License, or (at your option) any later version". Example: `src/deluge/deluge.cpp`, "Copyright © 2014-2023 Synthstrom Audible Limited".
- No file in `src/` carries a GPLv3-only header.
- Third-party files keep their own licences. 13 are Apache-2.0: the msfa/Dexed FM engine in `src/deluge/dsp/dx/`, "Copyright 2012 Google Inc.". Some are MIT, including Mutable-derived reverb code (below).
- About 320 of 1300 source files carry no licence text at all. They are covered by the repo's GPLv3 `LICENSE`.

GPLv3 §14 (in `LICENSE`): when a program says "or any later version", the recipient "ha[s] the option of following the terms and conditions either of that numbered version or of any later version".

### Precedent already in the tree

Upstream has already merged code from both Mutable Instruments and Dexed/msfa.

- `src/deluge/dsp/reverb/mutable.hpp` reads "Copyright 2023 Katherine Whitlock / Copyright 2014 Emilie Gillet". It is ported from Mutable Instruments reverb code, which is MIT like all of their STM32 code. It carries no licence text.
- `src/deluge/dsp/reverb/fx_engine.hpp` reads "Heavily based on Mutable Instrument's code, copyright 2014 Emilie Gillet".
- `src/deluge/dsp/reverb/cosine_oscillator.hpp` reads "Copyright 2014 Emilie Gillet. MIT License."
- `src/deluge/dsp/dx/*` keeps Google's full Apache-2.0 header.

The Mutable headers are thinner than the MIT licence asks for (see §4). Don't copy that pattern.

## 2. Compatibility rules (FSF / GNU)

From the GNU [licence list](https://www.gnu.org/licenses/license-list.html):

- **Expat/MIT**: "a lax, permissive non-copyleft free software license, compatible with the GNU GPL."
- **Apache 2.0**: "compatible with version 3 of the GNU GPL" (not with GPLv2).
- **MPL 2.0**: "Section 3.3 provides indirect compatibility between this license and the GNU GPL version 2.0, the GNU LGPL version 2.1, the GNU AGPL version 3, and all later versions of those licenses". Authors can opt out with the "Incompatible With Secondary Licenses" notice. Code that carries that notice "is not compatible with the GPL".

From the [GPL FAQ](https://www.gnu.org/licenses/gpl-faq.html):

- [#WhatDoesCompatMean](https://www.gnu.org/licenses/gpl-faq.html#WhatDoesCompatMean): compatible means "you can combine code released under the other license" with GPL code in one larger program.
- [#AllCompatibility](https://www.gnu.org/licenses/gpl-faq.html#AllCompatibility): GNU licences combine freely. The exception is code that is "only under an older version of a license with code that's under a newer version". In the matrix, GPLv3 code combined with "GPLv3 or later" code is "OK". The combination is distributed under GPLv3.

What this means here:

- **GPL-3.0-only into GPL-3.0-or-later works.** The combined work must be distributed under GPLv3, the only version both allow.
- Each Deluge file keeps its "or later" header. But the firmware binary could no longer move to a hypothetical GPLv4.
- That matters for upstreaming. Upstream would have to accept GPL-3.0-only files into a GPL-3.0-or-later tree. Nothing found in the repo forbids that; `README.md`, `docs/CONTRIBUTING.md` and `docs/GOVERNANCE.md` contain no licence policy. Still, it is a choice the maintainers may refuse. AGENTS.md calls upstreaming "not a goal", so this costs little.
- Material under GPL-3.0-or-later (Grids) or MIT (Marbles) carries no such cost.

## 3. Per source

### PERF-1: deamonSLINKY/PERF-1 @ `9a916c61ee`

- `LICENSE` is GPLv3. `LICENSING.md` says it is "free software under the GNU General Public License, version 3 only (`GPL-3.0-only`)".
- It is "a fork of HYP-1 by RedtailWorks, itself a fork of Charles Vestal's AMB-1". It also builds on Felucca's platform layer, which is GPL-3.0-only.
- `LICENSING.md` has a provenance table that names the author of each part. The parts most likely to be ported:
  - "Everything else in `firmware/src/{app,dsp}`, `tests/`", meaning the generator, credited to "AMB-1 (Charles Vestal), HYP-1 changes (RedtailWorks), PERF-1 changes (deamonSLINKY)", GPL-3.0-only.
  - The Dattorro plate in `amb.c`, from FoMni, GPL-3.0-only.
  - The FM engine and the "DX" factory voices (`fm6_rom.h`, `fm6_patches.h`). These are Melodee by Kerem Kilic, GPL-3.0-only, with headers `SPDX-License-Identifier: GPL-3.0-only / Copyright (C) 2026 Kerem Kilic (Ellic Studio)`. The synthesis follows Dexed (GPL-3.0-or-later) and MSFA (Apache-2.0). The Deluge already has its own MSFA/Dexed engine, so there is little reason to port this one.
- Tuna: "The ideas behind the bass line modes, the pad's comping rhythms, the genres' ghost notes and accents and the pluck's deja vu loop (written afresh: no code taken)". This is listed as "credit only". PERF-1 therefore contains no Tuna code that would need Tuna's notices. Credit Tuna if those ideas come over.
- **Verdict:** code may be ported. Keep each file's SPDX and copyright lines and add a "modified by … (date)" notice (GPLv3 §5a). Credit the authors named in PERF-1's table for the parts taken. For anything from PERF-1's `LICENSING.md` table, take its stated licence, not the repo-wide one.

### AMB-1: charlesvestal/fm1-amb @ `62a588ab10`

- `LICENSE` is MPL-2.0. `LICENSING.md`: "Until 0.9, AMB-1 was GPL-3.0-only … AMB-1's own code (the engine, the UI, the pieces) is relicensed by its author. AMB-1 0.9 and earlier stay under the GPL."
- Tags are `v0.9`, `v1.1`, `v1.2`, `v1.3`. The ticket says "MPL after 0.9". Precisely: **v0.9 itself is GPL-3.0-only, and MPL starts at v1.1.**
- Files carry `SPDX-License-Identifier: MPL-2.0 / Copyright (C) 2026 Charles Vestal`. Checked: `app/app.c`, `app/dsp/amb.c`.
- Neither those files nor `LICENSING.md` carry the Exhibit B "Incompatible With Secondary Licenses" notice. In the MPL text, Exhibit B appears only as the template inside `LICENSE` itself.
- The Dattorro plate inside `app/dsp/amb.c` came from FoMni (GPL-3.0-only). FoMni's `LICENSING.md` credits "Everything else in `firmware/src/{app,dsp}`" to FoMni itself, Charles Vestal's own project. So the plate can be relicensed by its author, and AMB-1's MPL claim holds as far as these documents show.
- MPL-2.0 §3.3: "You may create and distribute a Larger Work under terms of Your choice, provided that You also comply with the requirements of this License for the Covered Software." If the Covered Software is not marked Incompatible With Secondary Licenses, you may also distribute it under the Secondary Licence. §1.12 defines that to include the GPL "or any later versions".
- **Verdict:** code may be ported, from either licence era. For MPL code, choose one of two routes:
  - (a) Keep the ported files as separate MPL-2.0 files with the notices intact (§3.4) and offer their source (§3.2). The firmware as a whole is still distributed under the GPL. This is the simpler and more conservative route.
  - (b) Relicense those files to GPL under §3.3.
  
  MPL is file-level copyleft. Mixing MPL code into an existing GPL file makes that file's licensing muddy, so keep the ported code in its own files.
- `base/` (fm1-base, felucca-bsp) is MPL-2.0 per AMB-1's `LICENSING.md`. It is FM-1 platform code and unlikely to be ported.

### Mutable Instruments Grids: pichenettes/eurorack @ `f4cfa76970`, `grids/`

- Repo README: "Code (AVR projects): GPL3.0. Code (STM32F projects): MIT license. Hardware: cc-by-sa-3.0". The repo has no top-level `LICENSE`, so GitHub reports no licence.
- Grids is an AVR project. Its files, e.g. `grids/pattern_generator.cc` and `grids/resources.cc` (the generated drum-map tables), carry "Copyright 2011/2012 Emilie Gillet … either version 3 of the License, or (at your option) any later version". The table generator `grids/resources/resources.py` has the same header.
- This is the same licence as the Deluge. Porting adds no constraints.
- README, "Guidelines for derivative works": "Mutable Instruments is a registered trademark. The name 'Mutable Instruments' should not be used on any of the derivative works you create", and "We do not recommend you to keep the original name of the Mutable Instruments module". This is a trademark request, not a copyright condition. Crediting "based on Mutable Instruments Grids by Emilie Gillet" in a notice is normal attribution; naming the feature "Grids" is what to avoid.
- **Verdict:** code and tables may be ported. Keep the header and copyright, and add a modification notice.

### Mutable Instruments Marbles: same repo, `marbles/` (plus `stmlib`)

- Marbles is an STM32 project, so MIT. Files such as `marbles/random/t_generator.cc` and `random_generator.h` read "Copyright 2015 Emilie Gillet" with the MIT permission text.
- The text includes the condition: "The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software."
- Marbles depends on `stmlib`, a separate repo (pichenettes/stmlib). Its files carry the same MIT text, e.g. `utils/random.h`, "Copyright 2012 Emilie Gillet". GitHub reports it as `NOASSERTION` because there is no top-level LICENSE file.
- **Verdict:** code may be ported. Put the full MIT notice in each ported file; a one-line "MIT License." is not enough. The trademark guideline above also applies.

### Tuna: delirioux/tuna @ `7766ff8220`

- README: "Tuna is free software under the GNU General Public License, version 3 only". Tuna is "a modified version of **Felucca**", and its `LICENSING.md` is Felucca's (GPL-3.0-only, with listed exceptions).
- `tuna_gen.c` and the other `tuna_*` files carry `SPDX-License-Identifier: GPL-3.0-only`.
- The exceptions: `tuna_marbles*.c/.h` are `SPDX-License-Identifier: MIT`, "Original code Copyright 2015 Emilie Gillet, MIT License", ported to be bit-exact. Felucca's MIT engines under `firmware/engines/` and the Apache-2.0 msfa port are also exceptions.
- **Verdict:**
  - Tuna's own code may be ported. It is GPL-3.0-only, with the same consequences as PERF-1.
  - The Marbles port may be ported under MIT. Keep the MIT notice and credit Tuna's port.
  - Porting from Mutable's original instead of Tuna's C port avoids depending on Tuna's port being faithful. Both routes are allowed.

## 4. Notice duties when porting

These apply to whatever is ported:

1. **Keep existing notices.** GPLv3 §4 asks you to keep the licence and no-warranty notices intact, MPL §3.4 forbids removing them, and the MIT condition quoted above requires them. Prefer the original header over a one-line paraphrase.
2. **Mark modifications.** GPLv3 §5a: "prominent notices stating that you modified it, and giving a relevant date." A header line such as `Modified for Deluge Generative by …, 2026` does this. MPL-2.0 has no such rule, but the same line does no harm.
3. **Add your own GPL header to new files.** Use the Deluge's "or later" header for wholly new code. Use GPL-3.0-only for files derived from GPL-3.0-only sources, since they cannot be relicensed upward.
4. **Keep a provenance record.** Nothing in this repo lists third-party code; there is no `NOTICE` or `LICENSING.md`. PERF-1, AMB-1 and Tuna each keep a table of origin, licence and paths, and their verdicts above relied on it. A similar table here would carry the attribution duties for ported code. Whether to add one is an open question, below.
5. **Source for binaries.** GPLv3 §6 and MPL §3.2 require corresponding source with every binary release. A public fork that pushes its branches meets this as long as each release's source stays available.

## 5. Ideas versus code

Copyright protects expression, not ideas. US law, [17 U.S.C. §102(b)](https://www.law.cornell.edu/uscode/text/17/102): "In no case does copyright protection for an original work of authorship extend to any idea, procedure, process, system, method of operation, concept, principle, or discovery." EU Software Directive 2009/24/EC, Art. 1(2), says ideas and principles underlying a program are not protected; that text is cited from memory, because EUR-Lex returned nothing when fetched. The GPL can only license what copyright covers.

PERF-1 credits Tuna's ideas as "written afresh: no code taken". That is the "ideas only" route. It is open for any source, and it needs only a courtesy credit.

Close transliteration, such as Tuna's "structure, names and the order of every floating-point operation follow the original", is porting code, not taking ideas. It carries the licence duties.

## 6. PERF-1's bundled DX7 voices

PERF-1 bundles two voice sets.

1. **"DX" factory voices** (`fm6_rom.h`, `fm6_patches.h`): "Melodee's own DX7 voices". The headers read `GPL-3.0-only`, Copyright Kerem Kilic. Provenance is clear, and the voices can be ported under GPL-3.0-only.

2. **"DY" voices** (`fm6_dy.h`, listed per voice in `tools/fm6_dy.txt`): thirty voices plus a 32-voice test bank.
   - Most come from Bobby Blues' DX7 page: madFame, Pafreak's SYNTHOXICATED, Jexus, and magazine banks (Aftertouch US, Keyboards FR, Guitares & Claviers FR, E&MM / Music Technology UK, Keyboard US). One comes from Dave Benson's page (Jack Deckard).
   - PERF-1 rests the licensing on two site statements: "All patches published on this page become automatically free public domain material", and "a collection of public domain patches for the DX7".
   - Bobby Blues' page shows that the first statement covers material *contributed to that page*. For the main collection, the compiler says that since he downloaded the banks for free he "may consider it as public domain material", and offers to remove banks on objection. The magazine banks are "Most of them are readers' creations", which he typed in by hand from scanned issues.
   - So the public-domain status is the compiler's assumption, not a dedication by the patch authors or the magazines. The page footer also says "All Rights Reserved".
   - PERF-1 says it takes no voice from Yamaha's ROM banks; its generator rejects any voice whose operators match them.
   - Whether a DX7 patch (155 parameter values) is copyrightable at all is unsettled. This research does not try to answer it.

**Verdict on DY voices:** don't bundle them. The Deluge already loads DX7 banks at runtime ("load DX7 .syx file", `src/deluge/gui/ui/browser/dx_browser.cpp`), so users can bring their own.

## Open questions

- Should this repo add a `LICENSING.md` provenance table, modelled on PERF-1's, before the first ported file lands?
- Should GPL-3.0-only imports be avoided, or kept in separate files, to keep the generative code upstreamable under GPL-3.0-or-later?
- Should the upstream Mutable reverb headers (`mutable.hpp`, `fx_engine.hpp`, `cosine_oscillator.hpp`) be fixed to carry the full MIT notice? This is outside the generative scope.

## Sources

- This tree, `generative` @ `3ca05672`: `LICENSE`, `src/deluge/deluge.cpp`, `src/deluge/dsp/dx/fm_core.cpp`, `src/deluge/dsp/reverb/{mutable,fx_engine,cosine_oscillator}.hpp`, `src/deluge/gui/ui/browser/dx_browser.cpp`.
- [PERF-1 `LICENSING.md`](https://github.com/deamonSLINKY/PERF-1/blob/9a916c61ee/LICENSING.md), `LICENSE`, `firmware/src/dsp/{fm6_rom.h,fm6_patches.h,fm6_dy.h}`, `tools/fm6_dy.txt`.
- [AMB-1 `LICENSING.md`](https://github.com/charlesvestal/fm1-amb/blob/62a588ab10/LICENSING.md), `LICENSE` (MPL-2.0 §§1.5, 1.12, 3.2–3.4), `app/app.c`, `app/dsp/amb.c`, tags. [FoMni `LICENSING.md`](https://github.com/charlesvestal/fm1-fomni/blob/main/LICENSING.md).
- [eurorack README](https://github.com/pichenettes/eurorack/blob/f4cfa76970/README.md), `grids/pattern_generator.cc`, `grids/resources.cc`, `grids/resources/resources.py`, `marbles/random/t_generator.cc`, `.gitmodules`. [stmlib `utils/random.h`](https://github.com/pichenettes/stmlib/blob/master/utils/random.h).
- [Tuna README](https://github.com/delirioux/tuna/blob/7766ff8220/README.md), `LICENSING.md`, `CREDITS.md`, `firmware/src/tuna_gen.c`, `firmware/src/tuna_marbles.c`.
- GNU: [Various Licenses and Comments about Them](https://www.gnu.org/licenses/license-list.html); [GPL FAQ](https://www.gnu.org/licenses/gpl-faq.html) (#WhatDoesCompatMean, #AllCompatibility, #VersionThreeOrLater). [license-compatibility.html](https://www.gnu.org/licenses/license-compatibility.html) returned HTTP 429 and was not read.
- [Bobby Blues' DX7 patches page](https://bobbyblues.recup.ch/yamaha_dx7/dx7_patches.html); [17 U.S.C. §102](https://www.law.cornell.edu/uscode/text/17/102).
