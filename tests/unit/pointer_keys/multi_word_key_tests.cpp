// OrderedResizeableArrayWithMultiWordKey keyed on pointers, as Song::backedUpParamManagers is, at the host's own
// pointer width. On a 64-bit host the low words of heap addresses go past 0x80000000, which the Deluge's signed word
// compare can't order.

#include "CppUTest/TestHarness.h"
#include "util/container/array/ordered_resizeable_array_with_multi_word_key.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace {

// Laid out as Song's BackedUpParamManager: the two pointers are the key.
struct Entry {
	void* modControllable;
	void* clip;
	int32_t payload;
};

constexpr int32_t kKeyWords = 2 * sizeof(void*) / sizeof(uint32_t);

// As Song's backedUpParamManagerKey().
void makeKey(uint32_t* keyWords, void* modControllable, void* clip) {
	memcpy(&keyWords[0], &modControllable, sizeof(void*));
	memcpy(&keyWords[kKeyWords / 2], &clip, sizeof(void*));
}

uint32_t lowWord(void* pointer) {
	return (uint32_t)(uintptr_t)pointer;
}

// A small LCG, so every run tests the same keys.
struct Lcg {
	uint32_t state;
	uint32_t next() {
		state = state * 1664525u + 1013904223u;
		return state;
	}
};

// A fake address, 16-byte aligned. On a 64-bit host its low word spans the whole 32-bit range and its high word
// varies, like macOS heap addresses. On a 32-bit build it stays in the Deluge's SDRAM range, where the signed compare
// holds.
void* fakeAddress(Lcg& lcg) {
	uint32_t low = lcg.next() & ~0xFu;
	if constexpr (sizeof(void*) > sizeof(uint32_t)) {
		uint64_t high = 1 + (lcg.next() & 0x3);
		return (void*)(uintptr_t)((high << 32) | low);
	}
	else {
		return (void*)(uintptr_t)(0x0C000000u + (low & 0x03FFFFF0u));
	}
}

struct Owner {
	void* modControllable;
	std::vector<void*> clips; // nullptr is the NULL clip
};

} // namespace

TEST_GROUP(MultiWordKeyPointerTests){};

TEST(MultiWordKeyPointerTests, pointerKeysSortAndSearch) {
	OrderedResizeableArrayWithMultiWordKey array(sizeof(Entry), kKeyWords);
	Lcg lcg{12345};

	// 240 owners with distinct low words (key searches by the low word alone, as Song's do), each with 0-3 clips and
	// every other one with a NULL clip.
	std::vector<Owner> owners;
	std::vector<uint32_t> usedLowWords;
	while (owners.size() < 240) {
		void* modControllable = fakeAddress(lcg);
		if (std::find(usedLowWords.begin(), usedLowWords.end(), lowWord(modControllable)) != usedLowWords.end()) {
			continue;
		}
		usedLowWords.push_back(lowWord(modControllable));
		Owner owner{modControllable, {}};
		if (owners.size() % 2 == 0) {
			owner.clips.push_back(nullptr);
		}
		for (uint32_t c = lcg.next() % 4; c > 0; c--) {
			owner.clips.push_back(fakeAddress(lcg));
		}
		owners.push_back(owner);
	}

	if constexpr (sizeof(void*) > sizeof(uint32_t)) {
		// The point of the test: many low words at or above 0x80000000.
		int32_t high =
		    std::count_if(usedLowWords.begin(), usedLowWords.end(), [](uint32_t w) { return w >= 0x80000000u; });
		CHECK(high > 60);
	}

	// Insert every (owner, clip) in a scrambled order, as Song::backUpParamManager() does.
	struct Pending {
		void* modControllable;
		void* clip;
		int32_t payload;
	};
	std::vector<Pending> pending;
	for (auto& owner : owners) {
		for (void* clip : owner.clips) {
			pending.push_back({owner.modControllable, clip, (int32_t)pending.size()});
		}
	}
	for (size_t i = pending.size(); i > 1; i--) {
		std::swap(pending[i - 1], pending[lcg.next() % i]);
	}
	for (auto& p : pending) {
		uint32_t keyWords[kKeyWords];
		makeKey(keyWords, p.modControllable, p.clip);
		int32_t insertAt;
		CHECK_EQUAL(-1, array.searchMultiWordExact(keyWords, &insertAt));
		CHECK(array.insertAtIndex(insertAt) == Error::NONE);
		*(Entry*)array.getElementAddress(insertAt) = {p.modControllable, p.clip, p.payload};
	}
	CHECK_EQUAL((int32_t)pending.size(), array.getNumElements());

	// Every exact lookup finds its own entry.
	for (auto& p : pending) {
		uint32_t keyWords[kKeyWords];
		makeKey(keyWords, p.modControllable, p.clip);
		int32_t i = array.searchMultiWordExact(keyWords);
		CHECK(i != -1);
		CHECK_EQUAL(p.payload, ((Entry*)array.getElementAddress(i))->payload);
	}

	// Each owner's entries form one run: a first-word search finds its start, the NULL clip comes first in it, and
	// searchPastPointerWord() finds its end.
	for (auto& owner : owners) {
		int32_t first = array.search(lowWord(owner.modControllable), GREATER_OR_EQUAL);
		int32_t past = array.searchPastPointerWord(lowWord(owner.modControllable), first);
		CHECK_EQUAL((int32_t)owner.clips.size(), past - first);
		for (int32_t i = first; i < past; i++) {
			CHECK(((Entry*)array.getElementAddress(i))->modControllable == owner.modControllable);
		}
		if (!owner.clips.empty() && owner.clips[0] == nullptr) {
			CHECK(((Entry*)array.getElementAddress(first))->clip == nullptr);
		}
		// Searching only the run before it, from 0 up to its start, finds nothing of its own.
		CHECK_EQUAL(first, array.search(lowWord(owner.modControllable), GREATER_OR_EQUAL, 0, first));
	}
}
