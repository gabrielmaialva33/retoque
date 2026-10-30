// Synthetic host timing of the reverse-reference bookkeeping: legacy model (legacy.h) vs the sharded index.
// Not part of the suite's pass/fail; run with `python3 tests/restrack-resource-references/run.py --bench`.
// It times only the container work a descriptor rebind does (detach old resource, attach new) and the
// TrackResourceRelease fast path. It says nothing about D3D12 runtime cost or in-game frame time.

#include <resource_tracking/ResourceReferenceIndex.h>

#include "legacy.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

namespace
{
struct Resource
{
    char bytes[64];
};

struct Heap
{
};

using Index = ResourceReferenceIndex<Resource, Heap>;
using Legacy = LegacyReferences<Resource, Heap>;
using Clock = std::chrono::steady_clock;

// Two resources share `slots` descriptor slots; every op moves one slot to the other resource, as a
// descriptor copy into a ring heap does. Each resource holds ~slots/2 references throughout.
template <class Container> double Rebind(unsigned slots, unsigned operations)
{
    Container container;
    Resource resources[2];
    auto heap = std::make_shared<Heap>();
    auto make = [&] { return std::weak_ptr<Heap>(heap); };
    container.Register(&resources[0]);
    container.Register(&resources[1]);
    std::vector<unsigned char> owner(slots);
    for (unsigned slot = 0; slot < slots; ++slot)
    {
        owner[slot] = slot & 1;
        container.Attach(&resources[owner[slot]], 1, slot, make);
    }

    std::mt19937 random(7);
    const auto start = Clock::now();
    for (unsigned op = 0; op < operations; ++op)
    {
        const auto slot = random() % slots;
        container.Detach(&resources[owner[slot]], 1, slot);
        owner[slot] ^= 1;
        container.Attach(&resources[owner[slot]], 1, slot, make);
    }
    const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    return elapsed / operations;
}

// Threads rebinding their own slots between their own pair of resources (no shared resource), so the
// only shared state is the container's locking. Wall time per op across all threads.
template <class Container> double Parallel(unsigned threads, unsigned slots, unsigned operationsPerThread)
{
    Container container;
    std::vector<Resource> resources(2 * threads);
    auto heap = std::make_shared<Heap>();
    for (auto& resource : resources)
        container.Register(&resource);

    std::vector<std::thread> workers;
    const auto start = Clock::now();
    for (unsigned t = 0; t < threads; ++t)
        workers.emplace_back(
            [&, t]
            {
                auto make = [&] { return std::weak_ptr<Heap>(heap); };
                std::vector<unsigned char> owner(slots);
                for (unsigned slot = 0; slot < slots; ++slot)
                    container.Attach(&resources[2 * t], t, slot, make);
                std::mt19937 random(t);
                for (unsigned op = 0; op < operationsPerThread; ++op)
                {
                    const auto slot = random() % slots;
                    container.Detach(&resources[2 * t + owner[slot]], t, slot);
                    owner[slot] ^= 1;
                    container.Attach(&resources[2 * t + owner[slot]], t, slot, make);
                    (void) container.Contains(&resources[2 * t]);
                }
            });
    for (auto& worker : workers)
        worker.join();
    const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    return elapsed / (static_cast<double>(threads) * operationsPerThread);
}

template <class F> double Best(F&& measure)
{
    double best = 1e300;
    for (int repeat = 0; repeat < 5; ++repeat)
        best = std::min(best, measure());
    return best;
}
} // namespace

int main()
{
    std::printf("SYNTHETIC host timing, best of 5, ns per rebind (detach + attach)\n");
    std::printf("%-34s %12s %12s %9s\n", "scenario", "legacy", "index", "speedup");
    for (unsigned slots : { 16u, 256u, 2048u, 8192u, 32768u })
    {
        const unsigned operations = slots >= 8192 ? 20000 : 200000;
        const double legacy = Best([&] { return Rebind<Legacy>(slots, operations); });
        const double index = Best([&] { return Rebind<Index>(slots, operations); });
        std::printf("1 thread, %6u refs/resource      %12.1f %12.1f %8.1fx\n", slots / 2, legacy, index,
                    legacy / index);
    }

    for (unsigned threads : { 1u, 4u, 8u })
    {
        const double legacy = Best([&] { return Parallel<Legacy>(threads, 64, 200000); });
        const double index = Best([&] { return Parallel<Index>(threads, 64, 200000); });
        std::printf("%u threads, 64 refs, private res (wall) %9.1f %12.1f %8.1fx\n", threads, legacy, index,
                    legacy / index);
    }

    return 0;
}
