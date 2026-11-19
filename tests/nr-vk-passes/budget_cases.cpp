#include "pch.h"
#include "DlssNr_VkBudget.h"
#include <cassert>
#include <iostream>

using namespace DlssNr::VkChain;
constexpr uint64_t MiB = 1ull << 20, GiB = 1ull << 30;
static uint64_t now = 1000, latency = 0;
static unsigned queries = 0;
static VkPhysicalDeviceMemoryProperties memory {};
static uint64_t budget = 8 * GiB, usage = GiB;
static bool unwritten = false;
uint64_t Clock() { return now; }
void VKAPI_CALL Query(VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties2* out)
{
    assert(physical == (VkPhysicalDevice) 1);
    ++queries;
    now += latency;
    if (unwritten)
        return;
    out->memoryProperties = memory;
    auto* budgets = static_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(out->pNext);
    assert(budgets->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT);
    budgets->heapBudget[0] = budget;
    budgets->heapUsage[0] = usage;
}
int main()
{
    memory.memoryHeapCount = 2;
    memory.memoryTypeCount = 3;
    memory.memoryHeaps[0] = { 8 * GiB, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT };
    memory.memoryHeaps[1] = { 16 * GiB, 0 };
    memory.memoryTypes[0] = { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0 };
    memory.memoryTypes[1] = { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0 };
    memory.memoryTypes[2] = { VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 1 };
    std::vector<std::string> asked, supported { VK_EXT_MEMORY_BUDGET_EXTENSION_NAME }, add;
    assert(!EnableBudgetExtension(false, true, asked, supported, add) && add.empty());
    assert(!EnableBudgetExtension(true, false, asked, supported, add) && add.empty());
    assert(!EnableBudgetExtension(true, true, asked, {}, add) && add.empty());
    assert(EnableBudgetExtension(true, true, asked, supported, add) && add.size() == 1);
    asked = add;
    add.clear();
    assert(EnableBudgetExtension(true, true, asked, supported, add) && add.empty());
    BudgetSource source;
    source.Configure((VkPhysicalDevice) 1, true, Query, Clock);
    const TextureReservation textures[] = { { 0, 64 * MiB }, { 1, 128 * MiB } };
    BudgetKey key;
    key.deviceGeneration = source.Generation();
    BudgetGate gate;
    const auto off = gate.Check(key, source, 256 * MiB, textures);
    assert(off.legacy && queries == 0);
    key.count = 2;
    auto accepted = gate.Check(key, source, 256 * MiB, textures);
    assert(accepted.reason == BudgetReason::None && !accepted.log && queries == 1);
    assert(accepted.sample.heap == 0 && accepted.textures == 192 * MiB);
    accepted = gate.Check(key, source, 256 * MiB, textures);
    assert(accepted.reason == BudgetReason::None && queries == 2); // every accepted attempt is live
    usage = 7 * GiB;
    auto refused = gate.Check(key, source, 256 * MiB, textures);
    assert(refused.reason == BudgetReason::Headroom && refused.log && queries == 3);
    usage = 0;
    now += 100000;
    refused = gate.Check(key, source, 256 * MiB, textures);
    assert(refused.reason == BudgetReason::Headroom && !refused.log && queries == 3);
    key.settings[9].Intensity = .2f;
    key.individual = true;
    assert(gate.Check(key, source, 256 * MiB, textures).reason == BudgetReason::Headroom && queries == 3);
    key.settings[1].Intensity = .5f;
    assert(gate.Check(key, source, 256 * MiB, textures).reason == BudgetReason::None && queries == 4);

    const auto check = [&](BudgetReason reason, std::span<const TextureReservation> reservations)
    {
        ++key.width; // meaningful change, allowing exactly one attempt and one refusal log
        const auto answer = gate.Check(key, source, 256 * MiB, reservations);
        assert(answer.reason == reason);
        assert(answer.log == (reason != BudgetReason::None));
    };
    memory.memoryHeaps[1].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    check(BudgetReason::HeapShape, textures);
    memory.memoryHeaps[1].flags = 0;
    memory.memoryHeaps[0].flags = 0;
    check(BudgetReason::HeapShape, textures);
    memory.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    const TextureReservation mixed[] = { textures[0], { 2, MiB } };
    check(BudgetReason::Attribution, mixed);
    const TextureReservation unknown[] = { { UINT32_MAX, MiB } };
    check(BudgetReason::Attribution, unknown);
    const TextureReservation zero[] = { { 0, 0 } };
    check(BudgetReason::Attribution, zero);
    check(BudgetReason::Attribution, {});
    const TextureReservation overflow[] = { { 0, UINT64_MAX }, { 1, 1 } };
    check(BudgetReason::Overflow, overflow);
    budget = 0;
    check(BudgetReason::Query, textures);
    budget = 9 * GiB;
    check(BudgetReason::Query, textures);
    budget = 8 * GiB;
    usage = UINT64_MAX;
    check(BudgetReason::Query, textures);
    usage = 0;
    unwritten = true;
    check(BudgetReason::Query, textures);
    unwritten = false;
    latency = BudgetSource::MaxAgeMs + 1;
    check(BudgetReason::Stale, textures);
    latency = 0;
    auto sample = source.Read();
    assert(source.Fresh(sample));
    now += BudgetSource::MaxAgeMs + 1;
    assert(!source.Fresh(sample));
    now = sample.at - 1;
    assert(!source.Fresh(sample));
    memory.memoryHeapCount = VK_MAX_MEMORY_HEAPS + 1;
    check(BudgetReason::Query, textures);
    memory.memoryHeapCount = 2;
    const auto previous = source.Generation();
    source.Configure((VkPhysicalDevice) 1, true, Query, Clock);
    assert(source.Generation() != previous);
    check(BudgetReason::Attribution, textures); // old device generation cannot authorize a reused handle
    key.deviceGeneration = source.Generation();
    check(BudgetReason::None, textures);
    source.Configure((VkPhysicalDevice) 1, false, Query, Clock);
    auto count = queries;
    check(BudgetReason::Extension, textures);
    assert(queries == count);
    source.Configure((VkPhysicalDevice) 1, true, nullptr, Clock);
    check(BudgetReason::Query, textures);
    assert(queries == count);
    std::atomic<bool> lost { true };
    source.Configure((VkPhysicalDevice) 1, true, Query, Clock, &lost);
    check(BudgetReason::Query, textures);
    assert(queries == count);

    RetainedGenerations retained;
    assert(!retained.Reserve(1));
    const auto a = retained.Reserve(2), b = retained.Reserve(10);
    assert(a && b && a != b && !retained.Reserve(2));
    assert(!retained.Release(a, false, true) && !retained.Release(a, true, false));
    assert(!retained.Reserve(2));
    retained.Abandon(a);
    assert(!retained.Release(a, true, true));
    assert(retained.Release(b, true, true));
    const auto c = retained.Reserve(2);
    assert(c && c != a && c != b && !retained.Reserve(2));
    retained.Abandon(c);
    assert(!retained.Release(c, true, true) && !retained.Reserve(2));
    std::cout
        << "PASS: single-local-heap budget, fresh queries, exact texture attribution, latching and bounded retention\n";
}
