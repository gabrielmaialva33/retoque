#pragma once
#include <cstdint>
// Host fake: the menu teardown's two calls are recorded as events (definitions in fakes.cpp).
namespace DeferredRelease
{
void FlushOwner(uint64_t owner);
void RetireOwner(uint64_t owner, bool destroy);
} // namespace DeferredRelease
