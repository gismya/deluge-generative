# Implementation notes: Recipe files

Part of the [implementation notes](../implementation-notes.md). Paths are under `src/deluge/` unless they say
otherwise.

- The format is portable: `generative/core/recipe_file.*` writes through `RecipeFileWriter` (the firmware's forwards
  to the stock `XMLSerializer`, `firmware/recipe_files.cpp`; the harness's, `XmlText` in `recipe_file_spec.cpp`, lays
  the text out as the serializer does) and parses text with the shared tag reader `generative/core/xml_tags.h`, which
  palette override files use too. A new field is an attribute the parser reads with a default, so older files load.
- The stock `XMLSerializer::writeAttribute()` writes a value as it is, with no escaping: the core entity-encodes card
  paths before handing them over, and decodes them on read. Stock song files don't do this; Recipe files are only
  read by the core's parser, so that's fine.
- Names are portable too: `generative/perform/recipe_name.*` (beside `seedCode()`, which the default name uses). The
  Current Recipe remembers a `RecipeName`: its text and the stem length the letters go after, so a seed code ending in
  a letter is never mistaken for a variation. The firmware lists `RECIPES/` with `CardFolders` into a `NameFamily`.
  Stock's `nextDefaultName()` gives letters only to `SONG<digits>` names (others get ` 2`), so it isn't used.
- `CurrentRecipe::replaced()` clears the name: every genre pick, Regenerate (Compose's or Perform's, at its start step
  in the tick) and `reset recipe`. A Recipe file's load calls `replaced()` and then sets the file's name: the
  Compose screen's in `CurrentRecipe::load()`, Perform's in `Transitions::adoptPending()` from `pendingName`.
- A save copies the Current Recipe into `snapshot` (SDRAM) before its first card access: the tick runs in every card
  wait, so a transition's start step or a steering press could otherwise land between the attributes. It adopts the
  saved name only if `CurrentRecipe::generation` hasn't moved meanwhile.
- SAVE writes from the button handler, as stock's saves do, not as a runner job. It is refused while
  `runner.busy()`, inside a card routine or while `currentlyAccessingCard` (`recipeSaveAllowed()`). The full save UI
  checks again at its SAVE: a queued transition's preload may have started while it was open.
- The save UI (`gui/ui/save/save_recipe_ui.*`) is opened over Perform or the sound editor. Over Perform it runs
  `performView.graphicsRoutine()` and hands the sidebar and SYNC-SCALING to Perform, as the preset browser does; without
  that, swaps and transitions stall while it's open. It creates `RECIPES/` as it opens: the browser fails on a missing
  folder. It nominates a name no file has (`enteredText` set before `arrivedInNewFolder()`), which the browser keeps;
  a nominated name that exists would get stock's numeric variation instead.
- `firmware/card_text.*` reads a small card file whole (`readCardText()`): palette overrides and Recipe files share it.
- Test port: `check recipe <name>|*` reads a file back and compares it with the Current Recipe (`gen_recipe_check`);
  `*` is the remembered name. Scripts: `perform_recipe_save.txt`, `compose_recipe_save.txt`.
- A UI that refuses a press inside a card wait instead of deferring it (no REMIND_ME_OUTSIDE_CARD_ROUTINE) has to
  handle the release too: stock's `Browser::mainButtonAction()` sets `UI_MODE_HOLDING_BUTTON_POTENTIAL_LONG_PRESS` and
  the UI_SPECIFIC timer on a press made outside the wait, and only its release clears them. A release swallowed inside
  the wait left the mode set: the list wouldn't scroll and the next press did nothing (#416 R2).
- LOAD (#139): the browser is `gui/ui/load/load_recipe_ui.*` (`LoadRecipeUI`). Its `renderMainPads()` returns
  `qwertyVisible`, so once a scroll hides the QWERTY the UIs below draw the pads; `arrivedInNewFolder()` hides it
  too, so `opened()` sets it again after. It reads the chosen file from the button handler into a file-scope
  `Chosen`, which the transition confirm points at; `Transitions::submitPreload()` copies it into the job's own
  `LoadSlot`, so a refused request, or a later choice, can't change what a running or waiting job reads. LOAD needs
  no Current Recipe: it asks `recipeCardAccessAllowed()`, not `recipeSaveAllowed()` (which needs one; the first
  delugemu run of compose_recipe_load.txt hit BUSY on an empty Compose screen).
- Missing sounds: `replaceMissingSounds()` (`core/palette.*`) behind the `FileChecker` seam; the firmware's is
  `CardFiles` (`firmware/card_sounds.*`, `FatFS::stat`, a chunk boundary per check), the harness's `FakeCard`.
  `replaceMissingCardSounds()` (`current_recipe.*`) is the one call both LOAD paths make.
- Adding a member to `PerformDriver` moves `PerformAdapter::sink_` and trips the start step's mod-8 `static_assert`
  in `incomingStarts()`: `incomingKeepsMacros_` sits in the padding after `dropReported_`.
- Scripts: `perform_recipe_load.txt`, `compose_recipe_load.txt`, on the card `scripts/generative/fixtures/recipe_load`
  (its RECIPES/ files are hand-written; lanes left out have no pick, so nothing is replaced for them).
- `hold preload` also parks the Compose screen's card edits (`RollJob`, `gen_recipe_roll <roll> held`), so a script
  can BACK a LOAD or a roll while it runs (`compose_recipe_load.txt`). A playing LOAD's macros show as
  `gen_macro_landed` right after the start step: give the file macros unlike the live ones, or the check can't fail.
