/*
 * Copyright © 2026 Lars Johansson
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

// Perform (spec §13): the root UI while the Generator plays live. It has PLAY and SHIFT+PLAY, the Recipe-level gold
// knobs (F8 = Energy / Density, F8 again = Complexity / Motion; F1–F7 stay stock song FX), the tempo encoder and TAP
// for the Recipe tempo and swing (F4), the sidebar (part mute and solo, force, hold and End, spec §13.5), the OLED
// status line with a SELECT-turn peek at QUEUE and RECIPE, BACK tap to cancel the newest queued action, and the
// leave menu on BACK hold. The SONG grid page is the Arc map, with part focus on its rows and the ARC page on the OLED.
// The CLIP grid page shows the last part focused (spec §13.5): its takes on row 1, FREEZE and NEW SOUND on row 2, and
// its six sound controls as faders on rows 3–8, with the OLED's PART page showing its sound, take and controls. A
// focused part's F7 pressed again puts Thin / Drive on the gold knobs. SYNTH or KIT re-rolls the focused part's sound
// (SHIFT: its previous one), CROSS-SCREEN freezes it, and Y steps through its takes (Y push: key, scale and seed).
// SELECT push opens the Perform menu and SCALE its key and scale editor (spec §13.6), stock menus over Perform; SCALE
// blinks while a key change waits. The KEYBOARD grid page has the four macros as faders (tap = new centre, hold =
// a value that springs back), the phrase position and FILL pads, with the OLED's MACROS page. SHIFT + a main pad, or
// the CLIP page's SOUND EDITOR pad, opens a part's sound editor (the song FX with nothing focused) over Perform, its
// pads still live (spec §13.7).

#pragma once

#include "generative/core/arc_plan.h"
#include "generative/core/genre.h"
#include "generative/core/recipe.h"
#include "generative/core/types.h"
#include "generative/perform/arc_map.h"
#include "generative/perform/clip_page.h"
#include "generative/perform/grid_page.h"
#include "generative/perform/keyboard_page.h"
#include "gui/colour/rgb.h"
#include "gui/ui/root_ui.h"
#include "hid/button.h"
#include <cstdint>

class MenuItem;
namespace deluge::generative::firmware {
struct PartOutput;
enum class ReleaseAnswer : uint8_t;
} // namespace deluge::generative::firmware
class ModelStackWithAutoParam;

class PerformView final : public RootUI {
public:
	bool opened() override;
	void focusRegained() override;
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	/// BACK held: the leave menu, while playback continues (spec §13.8).
	ActionResult exitUI() override;
	void modButtonAction(uint8_t whichButton, bool on) override;
	void modEncoderAction(int32_t whichModEncoder, int32_t offset) override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;
	/// X turn: scrolls the Arc map over what played and what the Arc plans (spec §13.5).
	ActionResult horizontalEncoderAction(int32_t offset) override;
	/// Y turn: the focused part's take back or forward; forward past the newest makes a new take (spec §13.5).
	ActionResult verticalEncoderAction(int32_t offset, bool inCardRoutine) override;
	/// F4: the tempo encoder while Perform is the root UI, under any overlay too (spec §13.5, §13.7). Turn = Recipe
	/// tempo, SHIFT = swing, TAP held = swing interval, X held = stock sync nudge; LEARN+turn is swallowed. Under an
	/// external clock a tempo turn shows the tempo marked EXT and changes nothing.
	void tempoEncoderAction(int8_t offset, bool encoderButtonPressed, bool shiftButtonPressed);
	/// TAP, from the panel or MIDI: the Recipe tempo, gliding (spec §13.7).
	void setRecipeTempo(int32_t tempo);
	/// A macro CC on a MIDI-follow channel (CCs 114–117, spec §13.7): steers the macro's centre as a gold knob does,
	/// with stock's MIDI-follow popup when its DISPLAY PARAM setting is on.
	void steerMacroFromMidi(deluge::generative::Macro macro, int32_t units);
	/// SELECT turn: a 4 s peek at QUEUE or RECIPE below the status line (spec §13.5).
	void selectEncoderAction(int8_t offset) override;
	void graphicsRoutine() override;
	void notifyPlaybackBegun() override;
	void playbackEnded() override;
	bool renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                    uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth], bool drawUndefinedArea) override;
	bool renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                   uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]) override;
	void renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) override;
	UIType getUIType() override { return UIType::GENERATIVE_PERFORM; }

	/// LEAVE with nothing kept: tears Perform down as a runner job, which lands on a new blank song in session view.
	void leave();
	UIModControllableContext getUIModControllableContext() override { return UIModControllableContext::SONG; }
	bool getAffectEntire() override { return true; }

	/// The part on the gold knobs: the one held, or else the one locked; -1 for the Recipe level (spec §13.5).
	[[nodiscard]] int32_t focusedPart() const { return heldPart_ >= 0 ? heldPart_ : lockedPart_; }
	/// F15 (spec §13.5): while a focused part is on Thin / Drive, or the knobs are on F8's macros (under overlays too,
	/// F16), sets the knob rings to Thin and Drive's params or the macros' centres and returns true: Perform takes the
	/// rings, so stock's own refresh (View::setKnobIndicatorLevels) leaves them.
	bool takeKnobRings();
	/// The last part held or locked, which the CLIP page shows. -1 before any.
	[[nodiscard]] int32_t lastFocusedPart() const { return lastFocusedPart_; }

private:
	/// Opens a Perform menu over Perform (spec §13.6), stock style, as an overlay: `name` is its test-port name.
	ActionResult openMenu(MenuItem* menu, const char* name, bool inCardRoutine);
	/// The sound editor over Perform (spec §13.7) on `part`'s Sound (a Kit part's whole Kit), or with `part` -1 the
	/// song FX: at main pad (`x`, `y`)'s printed param, or with `x` -1 at its root menu. Opens it, or moves it there.
	ActionResult openSoundEditor(int32_t part, int32_t x, int32_t y);
	/// Closes a sound editor open on a part whose clip has left the roster (a sound swap, a new roster), once it is on
	/// top (spec §12.9, §13.7). A display reaction: releaseOutput() is what keeps the editor off a freed Output.
	void closeEditorOnRetiredPart();
	/// Closes the sound editor, on top and open on a part whose Output is going, with its test-port event.
	void closeRetiredEditor();
	/// The pump's question before `output` goes to the free job (generative/firmware/pump.h): moves the gold knobs
	/// off it, closes a sound editor open on it if the editor is on top, and answers Held while a UI sits over that
	/// editor.
	deluge::generative::firmware::ReleaseAnswer releaseOutput(const deluge::generative::firmware::PartOutput& output);
	/// Whether the sound editor over Perform is one Perform opened on a Sound or the song FX (openSoundEditor()), not a
	/// Perform menu: its pads stay live, where a menu closes on a pad press (spec §13.6).
	bool editorOpen_ = false;
	/// Every frame, under overlays too: puts back what a stock refresh changed (a menu's setModLedStates and its own
	/// LED setup), writing only LEDs that differ. The page LEDs and CROSS-SCREEN (assertPageLeds()); SCALE blinks while
	/// a key change waits (spec §13.5), F7 while Thin / Drive is on and F8 while Complexity / Motion is.
	void assertLeds();
	bool scaleBlinkShown_ = false; ///< The SCALE blink last reported (test builds) or started.
#if ENABLE_SYSEX_TEST
	char shownPageLeds_[48] = {}; ///< The last gen_page_leds text.
#endif
	/// The grid page (spec §13.5): SONG, CLIP and KEYBOARD switch it.
	using Page = deluge::generative::perform::GridPage;
	/// A main pad on the KEYBOARD page (spec §13.5): a macro fader's tap sets its centre and a hold plays its value
	/// until let go; a FILL pad holds FILL.
	void keyboardPadAction(int32_t x, int32_t y, bool on);
	/// Composes the KEYBOARD page (the macro faders, the phrase position, FILL) and redraws the rows that changed.
	/// Every frame: the Arc's values move step by step.
	void refreshKeyboardPage();
	/// A macro fader's pad let go: a tap keeps its value as the centre, a hold springs back (spec §13.5).
	void releaseMacroPad(deluge::generative::Macro macro, int32_t pad);
	/// Lets go of the KEYBOARD pads and SYNC-SCALING an overlay swallowed the releases of.
	void releaseLostHolds();
	/// Whether main pad (`x`, `y`) holds a KEYBOARD fader or FILL.
	[[nodiscard]] bool keyboardPadHeld(int32_t x, int32_t y) const;
	/// A macro's popup, as its gold knob shows it: its name and `units` in points.
	static void showMacroPopup(deluge::generative::Macro macro, int32_t units);
	/// A main pad on the CLIP page (spec §13.5): row 1 queues a take, row 2 is FREEZE and NEW SOUND (SHIFT: the
	/// previous sound), and a tap on rows 3–8 sets that control's home.
	void clipPadAction(int32_t x, int32_t y, bool on);
	/// NEW SOUND, SYNTH or KIT on `part`, with `previous` (SHIFT) its previous sound: the preload starts with the
	/// working animation, or the press is refused with a popup (spec §10.7, §12.10).
	void soundPress(int32_t part, bool previous);
	/// FREEZE or CROSS-SCREEN on `part`: it freezes or thaws on the next undecided step (spec §6.7).
	void freezePress(int32_t part);
	/// Y push: the key, scale and seed code as a popup (spec §6.5).
	void showKeyAndSeed();
	/// Whether a part is frozen in the piece playing (spec §6.7).
	[[nodiscard]] static bool partFrozen(int32_t part);
	/// Composes the CLIP page (takes, FREEZE, NEW SOUND and the faders as last read) and redraws the rows that
	/// changed. Every frame, so a queued take and a waiting sound blink.
	void refreshClipPage();
	/// The param `control`'s fader and Thin / Drive knob set on `part` (spec §13.5), built in `memory`; nullptr if the
	/// part has none (an FX part, or a preset that doesn't use the control).
	[[nodiscard]] static ModelStackWithAutoParam* leadParam(int32_t part, deluge::generative::SoundControl control,
	                                                        char* memory);
	/// A control's home and live value on a part, as knob positions (-64..64), for the faders and the PART page.
	struct ControlLevel {
		bool has = false; ///< The part's preset uses the control.
		int8_t home = 0;
		int8_t live = 0;
		bool operator==(const ControlLevel&) const = default;
	};
	/// `part`'s six controls, indexed by SoundControl.
	static void readControls(int32_t part, ControlLevel levels[deluge::generative::kNumSoundControls]);
	/// Reads the CLIP page's part's controls for its faders and the PART page, then redraws the CLIP page.
	void refreshFaders();
	/// Asks to redraw the main pad rows where `pads` differs from what they show.
	void showMainPads(const RGB pads[kDisplayHeight][kDisplayWidth]);
	/// A macro knob's ring level: its macro's centre (spec §13.5).
	[[nodiscard]] int32_t macroRing(int32_t whichModEncoder) const;
	/// A Thin / Drive knob's ring level (0–128): its lead param's value, or 0 where the part has none.
	[[nodiscard]] int32_t thinDriveRing(int32_t whichModEncoder) const;
#if ENABLE_SYSEX_TEST
	/// Test builds: reports whether the knob rings show Thin and Drive while that set is on, or the macros on F8,
	/// overlays included (gen_rings [macros ]ok, or stale if stock's levels came back).
	void checkKnobRings();
	char shownRings_[48] = {};    ///< The last gen_rings text, so only changes are reported.
	char shownFaders_[64] = {};   ///< The last gen_faders text.
	char shownClip_[64] = {};     ///< The last gen_clip text.
	char shownKeyboard_[64] = {}; ///< The last gen_keyboard text.
#endif
	/// A gold knob on the Thin / Drive set: the upper turns Thin's param, the lower Drive's, as stock knobs do.
	void thinDriveTurn(int32_t whichModEncoder, int32_t offset);
	/// Sets a lead param to `knobPos` with the stock popup: as its home (a CLIP fader tap) or as its value (a knob
	/// turn, whose home follows). Either way F3 sees a player edit.
	void setLeadParam(ModelStackWithAutoParam& stack, int32_t knobPos, bool asHome);
	/// A main pad on the SONG page: holding a part's row (rows 1–6) focuses it, a double tap locks it.
	void songPadAction(int32_t y, bool on);
	/// Puts the gold knobs on the focused part's Output, or the song (spec §13.5), and re-asserts the page LEDs.
	void bindKnobs();
	/// A FILL holder (SYNC-SCALING or a KEYBOARD FILL pad) down or up: while any is held, the song's fill mode and
	/// the SYNC-SCALING LED are on, with SHIFT latched as the first goes down (spec §8.7, §13.5).
	void holdFill(int32_t holder, bool on);
	/// Lets go of a held part row, if any; a locked part stays focused.
	void letGoOfHeldPart();
	/// Forgets the focus, held and locked: on stop, since focus is performance state (spec §13.1).
	void dropFocus();
	/// SONG, CLIP or KEYBOARD lit for the page that's on, AFFECT ENTIRE lit at the Recipe level.
	/// View::setModLedStates (from the stock calls Perform makes) sets them its own way, so every such call is followed
	/// by this (§19).
	void assertPageLeds();
	/// The Arc map's input as it stands, the plan as last worked out.
	[[nodiscard]] deluge::generative::perform::ArcMapInput arcMapInput() const;
	/// Works the Arc plan out again if a phrase line passed or the queue changed since the last time (spec §2.8).
	void refreshPlan();
	/// Composes the Arc map and asks to redraw only the pad rows whose colours changed (spec §13.5).
	void refreshArcMap();
	/// An Arc map cell's colour now, blinking included.
	[[nodiscard]] RGB arcMapColour(const deluge::generative::perform::ArcMapCell& cell, bool blinkOff) const;

	/// Whether the gold knobs are on F8, Perform's macro page.
	[[nodiscard]] bool onMacroPage() const;
	/// The macro a gold knob turns on F8's current set: the upper knob Energy or Complexity, the lower Density or
	/// Motion (spec §13.5).
	[[nodiscard]] deluge::generative::Macro knobMacro(int32_t whichModEncoder) const;
	/// Sets the knob rings for what the knobs turn: Thin / Drive, the macros, or stock's.
	void updateKnobIndicators();
	/// Whether the gold knobs are on a focused part's Thin / Drive set: its F7 pressed again (spec §13.5).
	[[nodiscard]] bool onThinDrivePage() const;
	/// The control a gold knob turns on the Thin / Drive set: the upper Thin, the lower Drive.
	static deluge::generative::SoundControl thinDriveControl(int32_t whichModEncoder);
	void showTempo();
	void showSwing();
	/// A pad in the sidebar (spec §13.5): MUTE/LAUNCH rows 1–6 mute or (X held) solo the parts, SHIFT at once;
	/// AUDITION/SECTION rows 1–3 force Groove, Break or Build, row 5 holds, row 8 is End.
	void sidebarPadAction(int32_t x, int32_t y, bool on);
	/// Composes the sidebar (generative/perform/sidebar.h), turns its cells into colours and asks to redraw only the
	/// rows that changed (spec §13.5). Test builds report a changed sidebar as gen_sidebar, unless `report` is false.
	void refreshSidebar(bool report = true);
	/// Forgets a hold on the End pad without arming or cancelling anything.
	void releaseEndPad();
	/// From graphicsRoutine(), every 15 ms: arms End once its pad has been held 0.6 s. A hold that an overlay
	/// interrupted is forgotten. Not the shared UI_SPECIFIC timer, which goes to whichever UI is current.
	void checkEndHold();

	/// The End pad is held, and whether this hold has armed End yet (spec §13.5).
	bool endHeld_ = false;
	bool endArmedThisHold_ = false;
	uint32_t endPressedAt_ = 0; ///< AudioEngine::audioSampleTimer at the press.

	/// BACK went down in this view, and its hold has opened the leave menu since: only a release of a press this view
	/// saw, without a hold, is a tap.
	bool backDown_ = false;
	bool backHeld_ = false;

	/// Part focus (spec §13.5): the part whose row is held, the locked one, -1 for none.
	int32_t heldPart_ = -1;
	/// The column of the pad holding heldPart_'s row: its release ends the hold on any page.
	int32_t heldX_ = -1;
	int32_t lockedPart_ = -1;
	int32_t lastFocusedPart_ = -1;
	/// The last press on a part row, for the double tap: its part and audioSampleTimer.
	int32_t lastTapPart_ = -1;
	uint32_t lastTapAt_ = 0;

	/// X since it went down: turned, or used for a solo or a clock nudge, so its release doesn't return the map to now.
	bool xUsed_ = false;
	/// X went down in this view: only a release of a press this view saw is a tap.
	bool xDown_ = false;
	/// What holds FILL: SYNC-SCALING and the KEYBOARD page's FILL pads, pressed in Perform with their releases not back
	/// here yet. An overlay opened over one may swallow its release, so focusRegained() lets go of those up by then.
	deluge::generative::perform::FillHolds fillHolds_;
	/// The KEYBOARD page's macro faders held, and since when (audioSampleTimer).
	deluge::generative::perform::MacroFaderHolds macroHolds_;
	/// soundSwaps.swaps() when the view last reacted to a swap: the gold knobs follow the part's new clip.
	uint32_t seenSwaps_ = 0;
	/// transitions.starts() when the view last reacted to a new roster: it drops the focus.
	uint32_t seenStarts_ = 0;
	/// Columns the Arc map is scrolled by X.
	int32_t scroll_ = 0;
	/// The now column's flash window, sampled once per step: the step it was sampled in, and the ticks a note-on must
	/// fall in to light its part (the step before and this one up to the sample).
	int32_t hitStep_ = -1;
	int32_t hitFrom_ = 0;
	int32_t hitTo_ = -1;

	/// The Arc plan as last worked out, and what it was worked out from: whether a piece plays, the phrase of its last
	/// decided step, and a hash of its steering state (the pending list, the hold, mutes and solos).
	deluge::generative::ArcPlan plan_;
	struct PlanKey {
		bool playing = false;
		uint32_t rosters = 0; ///< transitions.starts()
		int32_t decidedPhrase = -1;
		uint32_t steering = 0;
		bool operator==(const PlanKey&) const = default;
	};
	PlanKey planKey_;
	/// The pending list, soonest first, read with the plan: the status line, QUEUE and the map's blinks share it.
	deluge::generative::perform::QueuedAction queued_[deluge::generative::PendingList::kCapacity];
	int32_t queuedCount_ = 0;
	/// The main pads as last asked to render, so only changed rows redraw.
	RGB mainPads_[kDisplayHeight][kDisplayWidth] = {};
	/// Main pad redraws asked for since the heard phrase began (test builds report them at each phrase line, spec
	/// §13.5), and that phrase.
	uint32_t mainRedraws_ = 0;
	int32_t redrawsPhrase_ = -1;

	/// The sidebar as last asked to render, per row and column, so only changed rows redraw.
	RGB sidebar_[kDisplayHeight][kSideBarWidth] = {};
	/// The Section kind last seen playing, and its length in phrases (0 for a Groove), for the status line's count.
	int32_t lengthSection_ = -1;
	uint16_t lengthOccurrence_ = 0;
	int32_t sectionPhrases_ = 0;

	/// F8's two sets: false = Energy / Density, true = Complexity / Motion.
	bool secondMacroSet_ = false;
	/// The part whose F7 was pressed again, so its knobs are on Thin / Drive while it's focused on F7; -1 for none.
	int32_t thinDrivePart_ = -1;

	Page page_ = Page::Song;
	/// When the CLIP page's faders were last composed (audioSampleTimer): the live values move with every ramp, so
	/// they are read a few times a second.
	uint32_t fadersAt_ = 0;
	/// The CLIP page's part's controls as refreshFaders() last read them; the PART page draws these.
	ControlLevel faderLevels_[deluge::generative::kNumSoundControls] = {};

	/// The SELECT-turn peek below the status line (spec §13.5), and until when (audioSampleTimer).
	enum class Peek : uint8_t { None, Queue, Recipe };
	Peek peek_ = Peek::None;
	uint32_t peekUntil_ = 0;

	/// What the OLED shows, composed as text so graphicsRoutine() redraws only on a change.
	struct Screen {
		char left[24] = {};  ///< Section and count, HELD; or STOPPED
		char right[12] = {}; ///< the next queued action's tag, or bar.beat
		bool tag = false;    ///< `right` is a tag: drawn inverted
		bool ext = false;
		int16_t phraseBar = 0; ///< pixels
		static constexpr int32_t kLines = 3;
		char lines[kLines][24] = {};
		char lineRight[kLines][12] = {};
		/// The ARC page instead of the lines, while playing with no peek (spec §13.5).
		bool arc = false;
		deluge::generative::perform::ArcPage arcPage;
		/// The PART page instead: the CLIP page's part's sound and take, and its six controls as bars, home filled and
		/// live ticked.
		bool part = false;
		char sound[22] = {}; ///< the part's name and its preset's
		char take[deluge::generative::perform::kTakeLabelChars + 1] = {};
		ControlLevel controls[deluge::generative::kNumSoundControls];
		/// The MACROS page instead, with the KEYBOARD page (spec §13.5): each macro's centre filled, and a tick where
		/// the Arc has it (-1 when stopped), in macro units.
		bool macros = false;
		int16_t macroCentre[deluge::generative::kNumMacros] = {};
		int16_t macroArc[deluge::generative::kNumMacros] = {};
		bool operator==(const Screen&) const = default;
	};
	void compose(Screen& screen);
	/// The PART page's top line: the CLIP page's part's name and preset, and its take.
	void composePartTitle(Screen& screen) const;
	/// Composes the screen and, if anything changed, redraws the OLED.
	void refreshScreen();
	Screen screen_;
};

extern PerformView performView;
