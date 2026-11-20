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

	it("zeroes the whole-sound transpose of a pitched part's preset, and keeps an FX lane's and a Kit row's", _ {
		expect(partSoundSettings(part(Role::Bass, Flavour::Loop)).zeroTranspose).to_be_true();
		expect(partSoundSettings(part(Role::Lead, Flavour::Arp)).zeroTranspose).to_be_true();
		expect(partSoundSettings(part(Role::Chords, Flavour::Stab)).zeroTranspose).to_be_true();
		expect(partSoundSettings(part(Role::Pad, Flavour::Sustain)).zeroTranspose).to_be_true();
		for (int32_t l = 0; l < kNumLanes; l++) {
			expect(laneSoundSettings(static_cast<Lane>(l)).zeroTranspose).to_be_false();
		}
	});

	it("scales the volume knob by its position, so 50 percent halves it", _ {
		expect(scaleKnob(INT32_MIN, 50)).to_equal(INT32_MIN);
		expect(scaleKnob(0x12345678, 100)).to_equal(int32_t{0x12345678});
		// Full (50 on the knob) halves to the knob's middle (25), int32 0 within rounding.
		expect(scaleKnob(INT32_MAX, 50)).to_be_greater_than(int32_t{-2});
		expect(scaleKnob(INT32_MAX, 50)).to_be_less_than(int32_t{1});
		// 0x40000000 is 37.5 on the knob: half is 18.75, a quarter of the way up.
		expect(scaleKnob(0x40000000, 50)).to_equal(int32_t{INT32_MIN / 4});
	});

	it("starts Melodic and Acid techno's Pad (Acid techno's Drone) at half its preset's volume, every other part at the preset's", _ {
		for (const Genre* genre : {&genres::technoMelodic, &genres::technoAcid}) {
			for (const PartSpec& p : genre->parts) {
				uint8_t expected = p.role == Role::Pad ? 50 : 100;
				expect(partSoundSettings(p).volume).to_equal(expected);
			}
		}
	});

	it("starts Detroit techno's Strings at 60 percent and its Stab at 80 of their presets' volume, the rest at the preset's", _ {
		for (const PartSpec& p : genres::technoDetroit.parts) {
			const uint8_t expected = p.role == Role::Pad ? 60 : p.role == Role::Chords ? 80 : 100;
			expect(partSoundSettings(p).volume).to_equal(expected);
		}
	});

	it("starts Dub techno's Stab at its preset's volume, its Pad at 38 percent and every other part (each kit row) at 76 (#322)", _ {
		for (const PartSpec& p : genres::technoDub.parts) {
			const uint8_t expected = p.role == Role::Chords ? 100 : p.role == Role::Pad ? 38 : 76;
			expect(partSoundSettings(p).volume).to_equal(expected);
			for (const LaneSpec& lane : p.lanes) {
				expect(laneSoundSettings(p, lane.lane).volume).to_equal(expected);
			}
		}
	});

	it("keeps every Kit row at its preset's volume in every genre but Dub techno (#322)", _ {
		for (const Genre* genre : allGenres()) {
			if (genre == &genres::technoDub) {
				continue;
			}
			for (const PartSpec& p : genre->parts) {
				for (const LaneSpec& lane : p.lanes) {
					expect(laneSoundSettings(p, lane.lane).volume).to_equal(uint8_t{100});
				}
			}
		}
	});

	it("gives a Kit part's rows the part's volume, and the preset's when the part keeps it (spec §10.6)", _ {
		static constexpr LaneSpec kLanes[] = {{.lane = Lane::Kick}, {.lane = Lane::Snare}};
		static constexpr PartSpec kKit{.role = Role::Drums, .lanes = kLanes, .volume = 70};
		expect(laneSoundSettings(kKit, Lane::Kick).volume).to_equal(uint8_t{70});
		expect(laneSoundSettings(kKit, Lane::Snare).volume).to_equal(uint8_t{70});
		static constexpr PartSpec kPlain{.role = Role::Drums, .lanes = kLanes};
		expect(laneSoundSettings(kPlain, Lane::Kick).volume).to_equal(uint8_t{100});
		expect(laneSoundSettings(Lane::Kick).volume).to_equal(uint8_t{100});
	});

	it("carries a synth part's delay setup and each lane's row's, and none where the genre gives none (spec §10.6)", _ {
		constexpr DelaySetup kDotted{.on = true, .note = DelayNote::Eighth, .feel = DelayFeel::Dotted, .pingPong = true};
		constexpr DelaySetup kTriplet{.on = true, .note = DelayNote::Quarter, .feel = DelayFeel::Triplet, .analog = true};
		static constexpr LaneSpec kLanes[] = {{.lane = Lane::Snare, .delay = kTriplet}, {.lane = Lane::ClosedHat}};
		static constexpr PartSpec kKit{.role = Role::Drums, .lanes = kLanes, .delay = kDotted};
		PartSpec bass = part(Role::Bass, Flavour::Loop);
		expect(partSoundSettings(bass).delay.on).to_be_false();
		bass.delay = kDotted;
		expect(partSoundSettings(bass).delay == kDotted).to_be_true();
		expect(laneSoundSettings(kKit, Lane::Snare).delay == kTriplet).to_be_true();
		expect(laneSoundSettings(kKit, Lane::ClosedHat).delay.on).to_be_false(); // the kit's is at kit level, not a row's
		// The rest of a lane's settings are its lane's.
		expect(laneSoundSettings(kKit, Lane::ClosedHat).voiceMode == VoiceMode::Choke).to_be_true();
	});

	it("syncs a delay to its note at the song's tick magnitude, as a 16th is 8 - magnitude", _ {
		expect(delaySyncLevel(DelayNote::Sixteenth, 3)).to_equal(5); // the stock SYNC_LEVEL_16TH at the default
		expect(delaySyncLevel(DelayNote::Whole, 3)).to_equal(1);
		for (const int32_t magnitude : {1, 2, 3, 4}) {
			expect(delaySyncLevel(DelayNote::Sixteenth, magnitude)).to_equal(8 - magnitude);
			expect(delaySyncLevel(DelayNote::Eighth, magnitude)).to_equal(7 - magnitude);
		}
		expect(delaySyncLevel(DelayNote::Whole, 4)).to_equal(1); // held to the stock range
		expect(delaySyncLevel(DelayNote::ThirtySecond, -2)).to_equal(9);
	});
});

CPPSPEC_SPEC(sound_settings)
