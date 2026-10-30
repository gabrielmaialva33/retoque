// retoque/resource_tracking/ResourceReferenceIndex.h against the legacy single-mutex model (legacy.h):
// a randomized differential run, a concurrent stress run with an exact end-state check, and
// structural cases (shard spread, duplicates, Take order independence).

#include <resource_tracking/ResourceReferenceIndex.h>

#include "legacy.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

static std::atomic<int> failures { 0 };

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);                         \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace
{
struct Resource
{
    char bytes[64];
};

struct Heap
{
    uint64_t version = 0;
};

using Index = ResourceReferenceIndex<Resource, Heap>;
using Legacy = LegacyReferences<Resource, Heap>;

// (version, index, heap identity or null when expired), order-free
using Entry = std::tuple<uint64_t, unsigned, const Heap*>;

std::vector<Entry> Sorted(std::vector<Entry> entries)
{
    std::sort(entries.begin(), entries.end());
    return entries;
}

std::vector<Entry> StateOf(Index& index, Resource* resource, bool& registered)
{
    std::vector<Entry> entries;
    registered = index.ForEach(resource, [&](uint64_t version, unsigned slot, const std::weak_ptr<Heap>& heap)
                               { entries.emplace_back(version, slot, heap.lock().get()); });
    return Sorted(std::move(entries));
}

std::vector<Entry> StateOf(Legacy& legacy, Resource* resource, bool& registered)
{
    std::vector<Entry> entries;
    std::scoped_lock lock(legacy._trackedResourcesMutex);
    auto it = legacy._trackedResources.find(resource);
    registered = it != legacy._trackedResources.end();
    if (registered)
        for (const auto& slot : it->second)
            entries.emplace_back(slot.heapVersion, slot.index, slot.heap.lock().get());
    return Sorted(std::move(entries));
}

bool SameState(Index& index, Legacy& legacy, Resource* resource)
{
    bool indexRegistered = false, legacyRegistered = false;
    const auto a = StateOf(index, resource, indexRegistered);
    const auto b = StateOf(legacy, resource, legacyRegistered);
    return indexRegistered == legacyRegistered && a == b;
}

// Random operations over a small key space so duplicates, absent keys, reuse and expiry all occur often.
void Differential(uint64_t seed, unsigned operations)
{
    constexpr unsigned Resources = 24;
    constexpr unsigned Heaps = 6;
    constexpr unsigned Slots = 48;

    Index index;
    Legacy legacy;
    std::vector<Resource> resources(Resources);
    std::vector<std::shared_ptr<Heap>> heaps(Heaps);
    uint64_t nextVersion = 1;
    for (auto& heap : heaps)
        heap = std::make_shared<Heap>(Heap { nextVersion++ });

    std::mt19937_64 random(seed);
    unsigned mismatches = 0;
    for (unsigned op = 0; op < operations && mismatches < 10; ++op)
    {
        auto* resource = &resources[random() % Resources];
        const auto h = random() % Heaps;
        const auto slot = static_cast<unsigned>(random() % Slots);
        // Occasionally use the version of a heap that is no longer current (stale references)
        const uint64_t version = random() % 16 == 0 ? 1 + random() % nextVersion : heaps[h]->version;
        auto make = [&] { return std::weak_ptr<Heap>(heaps[h]); };

        switch (random() % 16)
        {
        case 0:
        case 1:
            index.Register(resource);
            legacy.Register(resource);
            break;
        case 2:
            mismatches += index.Contains(resource) != legacy.Contains(resource);
            break;
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
            mismatches += index.Attach(resource, version, slot, make) != legacy.Attach(resource, version, slot, make);
            break;
        case 8:
        case 9:
        case 10:
        case 11:
            index.Detach(resource, version, slot);
            legacy.Detach(resource, version, slot);
            break;
        case 12:
        {
            std::vector<Entry> taken, expected;
            for (const auto& [key, heap] : index.Take(resource))
                taken.emplace_back(key.heapVersion, key.index, heap.lock().get());
            for (const auto& entry : legacy.Take(resource))
                expected.emplace_back(entry.heapVersion, entry.index, entry.heap.lock().get());
            mismatches += Sorted(taken) != Sorted(expected);
            break;
        }
        case 13:
            // Heap replaced: the old object dies (weak references expire), a new version takes its place
            if (random() % 4 == 0)
                heaps[h] = std::make_shared<Heap>(Heap { nextVersion++ });
            break;
        default:
            // A hot resource: many slots at once, as descriptor copies into ring heaps produce
            for (unsigned k = 0; k < 8; ++k)
            {
                const auto many = static_cast<unsigned>(random() % Slots);
                mismatches += index.Attach(&resources[0], heaps[h]->version, many, make) !=
                              legacy.Attach(&resources[0], heaps[h]->version, many, make);
            }
            break;
        }

        mismatches += !SameState(index, legacy, resource);
        if (op % 997 == 0)
            for (auto& each : resources)
                mismatches += !SameState(index, legacy, &each);
    }

    for (auto& each : resources)
        mismatches += !SameState(index, legacy, &each);

    if (mismatches != 0)
        std::fprintf(stderr, "differential seed %" PRIu64 ": %u mismatches\n", seed, mismatches);
    CHECK(mismatches == 0);
}

void DifferentialCases()
{
    for (uint64_t seed : { 1ull, 0xD1010DBDull, 0x2BE1CF1Bull, 20261030ull })
        Differential(seed, 100000);
}

void Structure()
{
    Index index;
    std::vector<Resource> resources(4096);

    // Shard selection spreads aligned addresses
    std::vector<unsigned> perShard(Index::ShardCount);
    for (auto& resource : resources)
        perShard[Index::ShardOf(&resource)]++;
    const auto [low, high] = std::minmax_element(perShard.begin(), perShard.end());
    CHECK(*low > 0 && *high < 4 * 4096 / Index::ShardCount);

    auto heap = std::make_shared<Heap>(Heap { 7 });
    auto make = [&] { return std::weak_ptr<Heap>(heap); };
    auto* resource = &resources[0];

    CHECK(!index.Contains(resource));
    CHECK(!index.Attach(resource, 7, 0, make)); // not registered: rejected, nothing created
    CHECK(!index.Contains(resource));
    CHECK(!index.ForEach(resource, [](auto, auto, const auto&) {}));

    index.Register(resource);
    index.Register(resource);
    CHECK(index.Contains(resource));
    CHECK(index.Attach(resource, 7, 3, make));
    CHECK(index.Attach(resource, 7, 3, make)); // duplicate: accepted, not repeated
    unsigned count = 0;
    CHECK(index.ForEach(resource, [&](auto, auto, const auto&) { count++; }));
    CHECK(count == 1);

    // An emptied registration stays registered (the destruction callback is still installed)
    index.Detach(resource, 7, 3);
    index.Detach(resource, 7, 3);
    index.Detach(resource, 8, 3);
    CHECK(index.Contains(resource));
    count = 0;
    index.ForEach(resource, [&](auto, auto, const auto&) { count++; });
    CHECK(count == 0);

    // The factory runs only for a new reference
    unsigned made = 0;
    auto counting = [&]
    {
        made++;
        return std::weak_ptr<Heap>(heap);
    };
    index.Attach(resource, 7, 1, counting);
    index.Attach(resource, 7, 1, counting);
    CHECK(made == 1);

    // Removal from the middle of a large set, in an order unrelated to insertion
    for (unsigned slot = 0; slot < 20000; ++slot)
        index.Attach(resource, 9, slot, make);
    for (unsigned slot = 0; slot < 20000; slot += 3)
        index.Detach(resource, 9, slot);
    std::vector<unsigned> left;
    index.ForEach(resource,
                  [&](uint64_t version, unsigned slot, const auto&)
                  {
                      if (version == 9)
                          left.push_back(slot);
                  });
    std::sort(left.begin(), left.end());
    bool exact = left.size() == 20000 - 6667;
    for (size_t i = 0, slot = 1; exact && i < left.size(); ++i, slot += slot % 3 == 1 ? 1 : 2)
        exact = left[i] == slot;
    CHECK(exact);

    const auto taken = index.Take(resource);
    CHECK(taken.size() == left.size() + 1);
    CHECK(!index.Contains(resource));
    CHECK(index.Take(resource).empty());
    index.Detach(resource, 9, 1); // after Take: harmless
    CHECK(!index.Contains(resource));
}

// Threads race on shared resources (and so on shared shards) with disjoint slots each; a churn thread
// registers and takes its own resources while workers attach to them. Exact end state for stable resources.
void Stress(unsigned threads, unsigned operationsPerThread)
{
    constexpr unsigned Stable = 32;
    constexpr unsigned Churn = 8;
    constexpr unsigned SlotsPerThread = 256;

    Index index;
    std::vector<Resource> stable(Stable), churn(Churn);
    for (auto& resource : stable)
        index.Register(&resource);

    auto heap = std::make_shared<Heap>(Heap { 1 });
    std::atomic<bool> stop { false };

    std::thread churner(
        [&]
        {
            std::mt19937_64 random(99);
            while (!stop.load(std::memory_order_relaxed))
            {
                auto* resource = &churn[random() % Churn];
                if (random() % 2)
                    index.Register(resource);
                else
                    (void) index.Take(resource);
                (void) index.Contains(&stable[random() % Stable]);
            }
        });

    // expected[t][slot] = stable resource bound by thread t at that slot, or -1
    std::vector<std::vector<int>> expected(threads, std::vector<int>(SlotsPerThread, -1));
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t)
        workers.emplace_back(
            [&, t]
            {
                std::mt19937_64 random(0xC0FFEE + t);
                auto make = [&] { return std::weak_ptr<Heap>(heap); };
                const uint64_t version = 100 + t; // each thread owns its slot keys, as descriptor stripes do
                auto& mine = expected[t];
                for (unsigned op = 0; op < operationsPerThread; ++op)
                {
                    const auto slot = static_cast<unsigned>(random() % SlotsPerThread);
                    if (random() % 8 == 0)
                    {
                        auto* resource = &churn[random() % Churn];
                        if (index.Attach(resource, version, slot, make))
                            index.Detach(resource, version, slot);
                        continue;
                    }

                    // Rebind the slot the way SetByIndex does: detach the old resource, attach the new
                    if (mine[slot] >= 0)
                        index.Detach(&stable[mine[slot]], version, slot);
                    if (random() % 4 == 0)
                    {
                        mine[slot] = -1;
                        continue;
                    }
                    const int next = static_cast<int>(random() % Stable);
                    if (!index.Attach(&stable[next], version, slot, make))
                        failures++;
                    mine[slot] = next;
                }
            });

    for (auto& worker : workers)
        worker.join();
    stop = true;
    churner.join();

    unsigned wrong = 0;
    for (unsigned r = 0; r < Stable; ++r)
    {
        std::vector<std::pair<uint64_t, unsigned>> actual, want;
        index.ForEach(&stable[r],
                      [&](uint64_t version, unsigned slot, const auto&) { actual.emplace_back(version, slot); });
        for (unsigned t = 0; t < threads; ++t)
            for (unsigned slot = 0; slot < SlotsPerThread; ++slot)
                if (expected[t][slot] == static_cast<int>(r))
                    want.emplace_back(100 + t, slot);
        std::sort(actual.begin(), actual.end());
        std::sort(want.begin(), want.end());
        wrong += actual != want;
    }
    CHECK(wrong == 0);
}
} // namespace

int main(int argc, char** argv)
{
    const std::string selected = argc > 1 ? argv[1] : "all";
    const unsigned scale = argc > 2 ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10)) : 250000;

    const std::pair<const char*, void (*)(unsigned)> cases[] = {
        { "structure", [](unsigned) { Structure(); } },
        { "differential", [](unsigned) { DifferentialCases(); } },
        { "stress", [](unsigned perThread) { Stress(8, perThread); } },
    };

    bool ran = false;
    for (const auto& [name, run] : cases)
    {
        if (selected != "all" && selected != name)
            continue;
        const int before = failures;
        run(scale);
        std::printf("%s %s\n", failures == before ? "PASS" : "FAIL", name);
        ran = true;
    }

    if (!ran)
    {
        std::fprintf(stderr, "unknown case %s\n", selected.c_str());
        return 2;
    }

    return failures == 0 ? 0 : 1;
}
