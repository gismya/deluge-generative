// Perform's preset preview (spec §13.3, §13.5): while stopped, the preset under a browser's cursor loads into an
// Output of the part and plays one audition note; a move replaces it, and a stop, a start or a close frees it.

#include "generative/perform/preset_preview.h"
#include "generative/perform/sound_browser.h"

#include "cppspec.hpp"

#include <string>
#include <vector>

using namespace deluge::generative::perform;

namespace {

/// Records what the lifecycle asks for, as text, and answers as the test sets it.
struct FakePreview {
	std::vector<std::string> calls;
	bool busy = false;
	bool submits = true;
	uint8_t submitted = 0;

	[[nodiscard]] bool runnerBusy() const { return busy; }
	bool submitBuild(uint8_t token) {
		calls.push_back("submit");
		submitted = token;
		return submits;
	}
	void cancelBuild() { calls.push_back("cancel"); }
	void freeShown() { calls.push_back("free"); }
	void noteOn() { calls.push_back("on"); }
	void noteOff() { calls.push_back("off"); }

	[[nodiscard]] std::string take() {
		std::string text;
		for (const std::string& call : calls) {
			text += text.empty() ? call : " " + call;
		}
		calls.clear();
		return text;
	}
};

constexpr uint32_t kNote = kPresetPreviewNoteSamples;

} // namespace

// clang-format off
describe preset_preview("Perform's preset preview", $ {
	context("when presets preview", _ {
		it("previews presets only while stopped, at WHEN STOPPED or ALWAYS", _ {
			expect(previewsPresets(BrowserPreview::Off, false)).to_be_false();
			expect(previewsPresets(BrowserPreview::WhenStopped, false)).to_be_true();
			expect(previewsPresets(BrowserPreview::Always, false)).to_be_true();
			expect(previewsPresets(BrowserPreview::Off, true)).to_be_false();
			expect(previewsPresets(BrowserPreview::WhenStopped, true)).to_be_false();
			expect(previewsPresets(BrowserPreview::Always, true)).to_be_false();
		});
	});

	context("a preset under the cursor", _ {
		it("builds once the runner is free, then plays its note and lets it go after the note's length", _ {
			FakePreview fake;
			PresetPreviewLifecycle<FakePreview> preview(fake);
			fake.busy = true;
			preview.want();
			preview.service(0);
			expect(fake.take()).to_equal(std::string(""));
			fake.busy = false;
			preview.service(10);
			expect(fake.take()).to_equal(std::string("submit"));
			expect(preview.building()).to_be_true();
			expect(preview.built(fake.submitted, 100)).to_be_true();
			expect(fake.take()).to_equal(std::string("on"));
			expect(preview.building()).to_be_false();
			preview.service(100 + kNote - 1);
			expect(fake.take()).to_equal(std::string(""));
			preview.service(100 + kNote);
			expect(fake.take()).to_equal(std::string("off"));
			preview.service(100 + 3 * kNote);
			expect(fake.take()).to_equal(std::string(""));
		});

		it("keeps showing the preset it built: a frame doesn't build it again", _ {
			FakePreview fake;
			PresetPreviewLifecycle<FakePreview> preview(fake);
			preview.want();
			preview.service(0);
			preview.built(fake.submitted, 0);
			fake.calls.clear();
			preview.service(5);
			preview.service(6);
			expect(fake.take()).to_equal(std::string(""));
		});

		it("replaces the shown preset on a move: its note off and its Output freed before the next builds", _ {
			FakePreview fake;
			PresetPreviewLifecycle<FakePreview> preview(fake);
			preview.want();
			preview.service(0);
			preview.built(fake.submitted, 0);
			fake.calls.clear();
			preview.want();
			preview.service(1);
			expect(fake.take()).to_equal(std::string("off free submit"));
		});

		it("lets the build for the preset under the cursor run on over frames: only a move cancels it", _ {
			FakePreview fake;
			PresetPreviewLifecycle<FakePreview> preview(fake);
			preview.want();
			preview.service(0);
			preview.service(1);
			preview.service(2);
			expect(fake.take()).to_equal(std::string("submit"));
			expect(preview.built(fake.submitted, 3)).to_be_true();
			expect(fake.take()).to_equal(std::string("on"));
		});

		it("cancels a build the cursor has moved off, once, and builds the newest when it ends", _ {
			FakePreview fake;
			PresetPreviewLifecycle<FakePreview> preview(fake);
			preview.want();
			preview.service(0);
			const uint8_t first = fake.submitted;
			preview.want();
			preview.want();
			preview.service(1);
			preview.service(2);
			expect(fake.take()).to_equal(std::string("submit cancel"));
			preview.buildEnded();
			preview.service(3);
			expect(fake.take()).to_equal(std::string("submit"));
			expect(fake.submitted == first).to_be_false();
		});

		it("drops a build that ends after the cursor moved on: its Output isn't kept and no note plays", _ {
			FakePreview fake;
			PresetPreviewLifecycle<FakePreview> preview(fake);
			preview.want();
			preview.service(0);
			const uint8_t first = fake.submitted;
			fake.calls.clear();
			preview.want();
			expect(preview.built(first, 1)).to_be_false();
			expect(fake.take()).to_equal(std::string(""));
			preview.service(2);
			expect(fake.take()).to_equal(std::string("submit"));
		});

		it("doesn't try a failed build again until the cursor moves", _ {
			FakePreview fake;
			PresetPreviewLifecycle<FakePreview> preview(fake);
			preview.want();
			preview.service(0);
			preview.buildEnded();
			preview.service(1);
			preview.service(2);
			expect(fake.take()).to_equal(std::string("submit"));
			preview.want();
			preview.service(3);
			expect(fake.take()).to_equal(std::string("submit"));
		});

		it("tries again next frame when the runner refuses the build", _ {
			FakePreview fake;
			PresetPreviewLifecycle<FakePreview> preview(fake);
			fake.submits = false;
			preview.want();
			preview.service(0);
			expect(preview.building()).to_be_false();
			fake.submits = true;
			preview.service(1);
			expect(fake.take()).to_equal(std::string("submit submit"));
			expect(preview.building()).to_be_true();
		});
	});

	context("stop", _ {
		it("lets the note go and frees the shown Output at once", _ {
			FakePreview fake;
			PresetPreviewLifecycle<FakePreview> preview(fake);
			preview.want();
			preview.service(0);
			preview.built(fake.submitted, 0);
			fake.calls.clear();
			preview.stop();
			expect(fake.take()).to_equal(std::string("off free"));
			expect(preview.shows()).to_be_false();
		});

		it("frees an Output whose note has ended without letting it go twice", _ {
			FakePreview fake;
			PresetPreviewLifecycle<FakePreview> preview(fake);
			preview.want();
			preview.service(0);
			preview.built(fake.submitted, 0);
			preview.service(kNote);
			fake.calls.clear();
			preview.stop();
			expect(fake.take()).to_equal(std::string("free"));
		});

		it("cancels a running build, whose Output isn't kept when it ends, and builds nothing more", _ {
			FakePreview fake;
			PresetPreviewLifecycle<FakePreview> preview(fake);
			preview.want();
			preview.service(0);
			const uint8_t token = fake.submitted;
			fake.calls.clear();
			preview.stop();
			expect(fake.take()).to_equal(std::string("cancel"));
			expect(preview.building()).to_be_true();
			preview.stop();
			expect(fake.take()).to_equal(std::string(""));
			expect(preview.built(token, 1)).to_be_false();
			preview.service(2);
			expect(fake.take()).to_equal(std::string(""));
			expect(preview.building()).to_be_false();
		});

		it("does nothing with nothing shown or building", _ {
			FakePreview fake;
			PresetPreviewLifecycle<FakePreview> preview(fake);
			preview.stop();
			preview.service(0);
			expect(fake.take()).to_equal(std::string(""));
		});
	});
});

CPPSPEC_SPEC(preset_preview)
