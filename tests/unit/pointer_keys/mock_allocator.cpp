// A malloc-backed GeneralMemoryAllocator, for the container tests that run with the host's own pointer width. It never
// extends or shortens in place, which the containers handle by allocating again.

#include "CppUTest/TestHarness.h"
#include "memory/general_memory_allocator.h"
#include <cstdlib>

namespace {
// Each allocation keeps its size just before the address handed out.
constexpr size_t kHeader = 16;
} // namespace

MemoryRegion::MemoryRegion() = default;
GeneralMemoryAllocator::GeneralMemoryAllocator() = default;

void* GeneralMemoryAllocator::alloc(uint32_t requiredSize, bool, bool, void*) {
	auto* block = static_cast<char*>(malloc(requiredSize + kHeader));
	if (block == nullptr) {
		return nullptr;
	}
	*reinterpret_cast<uint32_t*>(block) = requiredSize;
	return block + kHeader;
}

void GeneralMemoryAllocator::dealloc(void* address) {
	if (address != nullptr) {
		free(static_cast<char*>(address) - kHeader);
	}
}

uint32_t GeneralMemoryAllocator::getAllocatedSize(void* address) {
	return *reinterpret_cast<uint32_t*>(static_cast<char*>(address) - kHeader);
}

uint32_t GeneralMemoryAllocator::shortenRight(void*, uint32_t) {
	return 0;
}

uint32_t GeneralMemoryAllocator::shortenLeft(void*, uint32_t, uint32_t) {
	return 0;
}

void GeneralMemoryAllocator::extend(void*, uint32_t, uint32_t, uint32_t* getAmountExtendedLeft,
                                    uint32_t* getAmountExtendedRight, void*) {
	*getAmountExtendedLeft = 0;
	*getAmountExtendedRight = 0;
}

void GeneralMemoryAllocator::checkStack(char const*) {
}

extern "C" {
void* delugeAlloc(unsigned int requiredSize, bool mayUseOnChipRam) {
	return GeneralMemoryAllocator::get().alloc(requiredSize, mayUseOnChipRam, false, nullptr);
}

void delugeDealloc(void* address) {
	GeneralMemoryAllocator::get().dealloc(address);
}
}

// What the containers reach for besides the allocator.
namespace AudioEngine {
bool bypassCulling = false;
}

char miscStringBuffer[256];

extern "C" void freezeWithError(char const* errmsg) {
	FAIL(errmsg);
}
