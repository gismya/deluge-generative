// What the Generator sets once when it loads a part's or lane's sound (spec §10.6, §5.8).

#include "generative/core/genre.h"
#include "generative/core/sound_settings.h"

#include "cppspec.hpp"

using namespace deluge::generative;

namespace {
constexpr PartSpec part(Role role, Flavour flavour) {
	return {.role = role, .flavour = flavour};
}
} // namespace

// clang-format off
describe sound_settings("Load-time sound settings", $ {
	it("sets each role's voice mode", _ {
		expect(partSoundSettings(part(Role::Bass, Flavour::Loop)).voiceMode == VoiceMode::Legato).to_be_true();
		expect(partSoundSettings(part(Role::Lead, Flavour::Loop)).voiceMode == VoiceMode::Legato).to_be_true();
		expect(partSoundSettings(part(Role::Lead, Flavour::Arp)).voiceMode == VoiceMode::Poly).to_be_true();
		expect(partSoundSettings(part(Role::Lead, Flavour::Motif)).voiceMode == VoiceMode::Poly).to_be_true();
		SoundSettings chords = partSoundSettings(part(Role::Chords, Flavour::Stab));
		expect(chords.voiceMode == VoiceMode::Poly).to_be_true();
		expect(chords.maxVoices).to_equal(uint8_t{4});
		expect(partSoundSettings(part(Role::Pad, Flavour::Sustain)).maxVoices).to_equal(uint8_t{4});
		expect(partSoundSettings(part(Role::Bass, Flavour::Loop)).maxVoices).to_equal(uint8_t{0}); // as saved
	});

	it("chokes the hat pair and only the hat pair", _ {
		for (int32_t l = 0; l < kNumLanes; l++) {
			auto lane = static_cast<Lane>(l);
			bool hat = lane == Lane::ClosedHat || lane == Lane::OpenHat;
			expect(laneSoundSettings(lane).voiceMode == VoiceMode::Choke).to_equal(hat);
		}
		expect(laneSoundSettings(Lane::Kick).voiceMode == VoiceMode::Auto).to_be_true();
	});

	it("ranks voice priority High for Drums and Bass, Medium for Lead and Chords, Low for Pad and FX", _ {
		expect(laneSoundSettings(Lane::Kick).priority == VoicePriorityLevel::High).to_be_true();
		expect(partSoundSettings(part(Role::Bass, Flavour::Loop)).priority == VoicePriorityLevel::High).to_be_true();
		expect(partSoundSettings(part(Role::Lead, Flavour::Loop)).priority == VoicePriorityLevel::Medium).to_be_true();
		expect(partSoundSettings(part(Role::Chords, Flavour::Stab)).priority == VoicePriorityLevel::Medium).to_be_true();
		expect(partSoundSettings(part(Role::Pad, Flavour::Sustain)).priority == VoicePriorityLevel::Low).to_be_true();
		expect(laneSoundSettings(Lane::Riser).priority == VoicePriorityLevel::Low).to_be_true();
	});

	it("gives sliding parts a portamento default and every other part portamento 0", _ {
		SoundSettings bass = partSoundSettings(part(Role::Bass, Flavour::Loop));
		expect(bass.slides).to_be_true();
		expect(bass.portamento).to_be_greater_than(uint8_t{0});
		expect(partSoundSettings(part(Role::Lead, Flavour::Loop)).slides).to_be_true();
		expect(partSoundSettings(part(Role::Lead, Flavour::Arp)).slides).to_be_false();
		expect(partSoundSettings(part(Role::Chords, Flavour::Stab)).slides).to_be_false();
	});
});

CPPSPEC_SPEC(sound_settings)
