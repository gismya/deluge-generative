// The sidebar (spec §13.5): the parts' mutes and solos on the MUTE/LAUNCH column, the forces, the hold and End on
// AUDITION/SECTION. Composed from a stepped piece as the Perform view composes it, and described as the test port's
// gen_sidebar text from the same cells.

#include "generative/perform/sidebar.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <string>

using namespace harness;
using namespace deluge::generative::perform;

namespace {

using Look = SidebarCell::Look;
constexpr int32_t kDrums = 0;
constexpr int32_t kAcid = 1;
constexpr int32_t kBreakRow = 1;
constexpr int32_t kBuildRow = 2;

/// A Techno piece played step by step, with the sidebar composed as the Perform view does.
struct Piece : StepwisePiece<> {
	bool endHeld = false;

	explicit Piece(bool skipIntro = true) : StepwisePiece({.seed = 3, .skipIntro = skipIntro}) {}
	SidebarInput input() {
		SidebarInput in;
		in.piece = driver.playing();
		in.parts = partCount(genres::techno);
		in.endHeld = endHeld;
		if (const Event* section = lastSection(); section != nullptr) {
			in.sounding = true;
			in.playing = section->section;
		}
		return in;
	}
	Sidebar sidebar() {
		Sidebar out;
		composeSidebar(input(), out);
		return out;
	}
	std::string text() {
		char out[kSidebarTextChars + 1];
		describeSidebar(sidebar(), out);
		return out;
	}
	const SidebarCell mute(int32_t part) { return sidebar().cell[part][kSidebarMuteColumn]; }
	const SidebarCell section(int32_t row) { return sidebar().cell[row][kSidebarSectionColumn]; }
};

Command solo(uint8_t part, bool now = false) {
	return {CommandKind::SetSolo, part, now ? kPressNow : kPress};
}

} // namespace

// clang-format off
describe sidebar("The sidebar", $ {
	it("shows a part per MUTE row from the top, the rows past the roster dark", _ {
		Piece piece;
		piece.decideBefore(4);
		const Sidebar sidebar = piece.sidebar();
		for (int32_t row = 0; row < Sidebar::kRows; row++) {
			expect(sidebar.cell[row][kSidebarMuteColumn].look).to_equal(row < 3 ? Look::Active : Look::Off);
			expect(sidebar.cell[row][kSidebarMuteColumn].blink).to_be_false();
		}
		expect(piece.text()).to_equal(std::string("mute=aaa force=- hold=0 end=-"));
	});

	it("shows no more parts than it has MUTE rows", _ {
		SidebarInput in;
		in.parts = Sidebar::kRows + 4;
		Sidebar sidebar;
		composeSidebar(in, sidebar);
		for (const auto& row : sidebar.cell) {
			expect(row[kSidebarMuteColumn].look).to_equal(Look::Active);
		}
	});

	it("lays out AUDITION/SECTION: Groove, Break and Build on rows 1–3, the hold on row 5, End on row 8", _ {
		Piece piece;
		piece.decideBefore(4);
		const Sidebar sidebar = piece.sidebar();
		const SectionKind kinds[] = {SectionKind::Groove, SectionKind::Break, SectionKind::Build};
		for (int32_t row = 0; row < 3; row++) {
			expect(sidebar.cell[row][kSidebarSectionColumn].look).to_equal(Look::Section);
			expect(sidebar.cell[row][kSidebarSectionColumn].kind).to_equal(kinds[row]);
		}
		expect(sidebar.cell[3][kSidebarSectionColumn].look).to_equal(Look::Off);
		expect(sidebar.cell[4][kSidebarSectionColumn].look).to_equal(Look::Hold);
		expect(sidebar.cell[5][kSidebarSectionColumn].look).to_equal(Look::Off);
		expect(sidebar.cell[6][kSidebarSectionColumn].look).to_equal(Look::Off);
		expect(sidebar.cell[7][kSidebarSectionColumn].look).to_equal(Look::Section);
		expect(sidebar.cell[7][kSidebarSectionColumn].kind).to_equal(SectionKind::Outro);
	});

	it("lights the pad of the Section that plays, once it is heard", _ {
		Piece piece;
		expect(piece.section(0).bright).to_be_false(); // nothing heard yet
		piece.runToStepLine(4);
		expect(piece.section(0).bright).to_be_true(); // the Groove
		expect(piece.section(kBreakRow).bright).to_be_false();
		expect(piece.section(kSidebarEndRow).bright).to_be_false();
	});

	it("blinks a mute waiting for its bar line, then shows the part muted", _ {
		Piece piece;
		piece.decideBefore(4);
		piece.pushAndDecide(mute(kAcid));
		expect(piece.mute(kAcid).look).to_equal(Look::Active);
		expect(piece.mute(kAcid).blink).to_be_true();
		expect(piece.mute(kDrums).blink).to_be_false();
		expect(piece.text()).to_equal(std::string("mute=aa*a force=- hold=0 end=-"));
		piece.decideBefore(kStepsPerBar + 1);
		expect(piece.mute(kAcid).look).to_equal(Look::Muted);
		expect(piece.mute(kAcid).blink).to_be_false();
		expect(piece.text()).to_equal(std::string("mute=ama force=- hold=0 end=-"));
	});

	it("mutes at once with SHIFT: nothing blinks", _ {
		Piece piece;
		piece.decideBefore(4);
		piece.pushAndDecide(mute(kAcid, true));
		expect(piece.mute(kAcid).look).to_equal(Look::Muted);
		expect(piece.mute(kAcid).blink).to_be_false();
	});

	it("shows a solo, and dims the parts it solos out", _ {
		Piece piece;
		piece.decideBefore(4);
		piece.pushAndDecide(solo(kAcid));
		expect(piece.text()).to_equal(std::string("mute=aa*a force=- hold=0 end=-")); // waiting for its bar line
		piece.decideBefore(kStepsPerBar + 1);
		expect(piece.mute(kAcid).look).to_equal(Look::Solo);
		expect(piece.mute(kDrums).look).to_equal(Look::Dim);
		expect(piece.text()).to_equal(std::string("mute=dsd force=- hold=0 end=-"));
	});

	it("puts solo over mute: a soloed part shows its solo, a soloed-out muted part stays red", _ {
		Piece piece;
		piece.decideBefore(4);
		piece.pushAndDecide(mute(kDrums, true));
		piece.pushAndDecide(mute(kAcid, true));
		piece.pushAndDecide(solo(kAcid, true));
		expect(piece.mute(kAcid).look).to_equal(Look::Solo); // muted and soloed: the solo shows
		expect(piece.mute(kDrums).look).to_equal(Look::Muted); // soloed out, and muted: red, not dim
		expect(piece.mute(2).look).to_equal(Look::Dim);
		expect(piece.text()).to_equal(std::string("mute=msd force=- hold=0 end=-"));
	});

	it("dims a part the Arc has silenced", _ {
		Piece piece;
		piece.decideBefore(4);
		piece.pushAndDecide({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0});
		piece.decideBefore(8 * kStepsPerBar + 1); // the Break keeps no ACID
		expect(piece.driver.playing()->partSilenced(kAcid)).to_be_true();
		expect(piece.mute(kAcid).look).to_equal(Look::Dim);
		expect(piece.mute(kDrums).look).to_equal(Look::Active);
	});

	it("blinks a force waiting for its line, on its own pad only, then lights the Section it brought", _ {
		Piece piece;
		piece.decideBefore(4);
		piece.pushAndDecide({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Break), 0});
		expect(piece.section(kBreakRow).blink).to_be_true();
		expect(piece.section(0).blink).to_be_false();
		expect(piece.section(kBuildRow).blink).to_be_false();
		expect(piece.text()).to_equal(std::string("mute=aaa force=B hold=0 end=-"));
		piece.runToStepLine(8 * kStepsPerBar);
		expect(piece.section(kBreakRow).blink).to_be_false();
		expect(piece.section(kBreakRow).bright).to_be_true();
		expect(piece.section(0).bright).to_be_false();
		expect(piece.text()).to_equal(std::string("mute=ada force=- hold=0 end=-"));
	});

	it("names a waiting Groove G and a waiting Build U", _ {
		Piece piece;
		piece.decideBefore(4);
		piece.pushAndDecide({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Build), 0});
		expect(piece.section(kBuildRow).blink).to_be_true();
		expect(piece.text()).to_equal(std::string("mute=aaa force=U hold=0 end=-"));
		piece.pushAndDecide({CommandKind::ForceSection, static_cast<uint8_t>(SectionKind::Groove), 0});
		expect(piece.section(0).blink).to_be_true();
		expect(piece.section(kBuildRow).blink).to_be_false(); // the later force took the slot
		expect(piece.text()).to_equal(std::string("mute=aaa force=G hold=0 end=-"));
	});

	it("lights the hold pad while the Arc is held", _ {
		Piece piece;
		piece.decideBefore(4);
		expect(piece.section(kSidebarHoldRow).bright).to_be_false();
		piece.pushAndDecide({CommandKind::SetHold, 0, kPress});
		expect(piece.section(kSidebarHoldRow).bright).to_be_true();
		expect(piece.section(kSidebarHoldRow).blink).to_be_false();
		expect(piece.text()).to_equal(std::string("mute=aaa force=- hold=1 end=-"));
		piece.pushAndDecide({CommandKind::SetHold, 0, kPress});
		expect(piece.text()).to_equal(std::string("mute=aaa force=- hold=0 end=-"));
	});

	it("blinks End while it is queued, and lights it once the Outro plays", _ {
		Piece piece;
		piece.decideBefore(4);
		piece.pushAndDecide({CommandKind::End, 0, 1});
		expect(piece.section(kSidebarEndRow).blink).to_be_true();
		expect(piece.section(kSidebarEndRow).bright).to_be_false();
		expect(piece.section(0).blink).to_be_false(); // End shares the force's slot, and is no force
		expect(piece.text()).to_equal(std::string("mute=aaa force=- hold=0 end=q"));
		piece.runToStepLine(8 * kStepsPerBar);
		expect(piece.section(kSidebarEndRow).blink).to_be_false();
		expect(piece.section(kSidebarEndRow).bright).to_be_true();
		expect(piece.text()).to_equal(std::string("mute=aaa force=- hold=0 end=-"));
	});

	it("lights End while its pad is held, queued or not", _ {
		Piece piece;
		piece.decideBefore(4);
		piece.endHeld = true;
		expect(piece.section(kSidebarEndRow).bright).to_be_true();
		expect(piece.section(kSidebarEndRow).blink).to_be_false();
		piece.pushAndDecide({CommandKind::End, 0, 1});
		expect(piece.section(kSidebarEndRow).bright).to_be_true();
		expect(piece.section(kSidebarEndRow).blink).to_be_true();
	});

	it("is fresh while stopped: the parts active, nothing queued, held or lit", _ {
		Piece piece;
		piece.decideBefore(4);
		piece.pushAndDecide(mute(kAcid, true));
		piece.pushAndDecide({CommandKind::SetHold, 0, kPress});
		piece.pushAndDecide({CommandKind::End, 0, 1});
		SidebarInput in = piece.input();
		in.piece = nullptr; // stopped
		in.sounding = false;
		Sidebar sidebar;
		composeSidebar(in, sidebar);
		char text[kSidebarTextChars + 1];
		describeSidebar(sidebar, text);
		expect(std::string(text)).to_equal(std::string("mute=aaa force=- hold=0 end=-"));
		expect(sidebar.cell[0][kSidebarSectionColumn].bright).to_be_false();
		expect(sidebar.cell[kSidebarHoldRow][kSidebarSectionColumn].look).to_equal(Look::Hold);
	});

	// Cells set by hand: no genre has six parts yet. This checks only that the longest text fits its buffer.
	it("fits the longest text in its buffer: six parts, each with a waiting press", _ {
		Sidebar sidebar;
		for (int32_t row = 0; row < kMaxParts; row++) {
			sidebar.cell[row][kSidebarMuteColumn] = {.look = Look::Dim, .blink = true};
		}
		char text[kSidebarTextChars + 1];
		describeSidebar(sidebar, text);
		expect(std::string(text)).to_equal(std::string("mute=d*d*d*d*d*d* force=- hold=0 end=-"));
	});
});

CPPSPEC_SPEC(sidebar)
