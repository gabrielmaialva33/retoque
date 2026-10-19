#include "pch.h"
#include "DeferredRelease.h"

#include <State.h>

#include <mutex>
#include <vector>
#include <unordered_set>

namespace
{
constexpr uint64_t MinPresents = 8;
constexpr auto MinAge = std::chrono::milliseconds(500);

// Without hooked presents only the age tells when the GPU is done
constexpr auto FallbackAge = std::chrono::seconds(2);

struct Entry
{
    std::function<void()> destroy;
    uint64_t owner = 0;
    uint64_t present = 0;
    std::chrono::steady_clock::time_point time;
};

// Serializes execution as well as extraction: a device cannot die while Collect has detached its callbacks.
// Recursive for API callbacks which re-enter collection on the same thread.
std::recursive_mutex executionMutex;
std::mutex mutex;
std::unordered_set<uint64_t> retiredOwners;
std::vector<Entry> entries;
uint64_t presentCounter = 0;
} // namespace

void DeferredRelease::Enqueue(std::function<void()> destroy, uint64_t owner)
{
    if (!destroy)
        return;

    // The process is going away, the OS cleans up
    if (State::Instance().isShuttingDown)
        return;

    std::lock_guard lock(mutex);
    if (owner != 0 && retiredOwners.contains(owner))
        return;
    entries.push_back({ std::move(destroy), owner, presentCounter, std::chrono::steady_clock::now() });
}

void DeferredRelease::DropOwnersExcept(uint64_t keep)
{
    std::lock_guard executionLock(executionMutex);
    std::lock_guard lock(mutex);

    const auto dropped =
        std::erase_if(entries, [keep](const Entry& entry) { return entry.owner != 0 && entry.owner != keep; });

    if (dropped > 0)
        LOG_WARN("Dropped {} deferred objects of a previous device", dropped);
}

void DeferredRelease::RegisterOwner(uint64_t owner)
{
    std::lock_guard lock(mutex);
    retiredOwners.erase(owner);
}

static void FinishOwner(uint64_t owner, bool destroy, bool retire)
{
    if (owner == 0)
        return;

    std::lock_guard executionLock(executionMutex);
    std::vector<std::function<void()>> pending;
    {
        std::lock_guard lock(mutex);
        if (retire)
            retiredOwners.insert(owner);
        std::erase_if(entries,
                      [&](Entry& entry)
                      {
                          if (entry.owner != owner)
                              return false;
                          if (destroy && !State::Instance().isShuttingDown)
                              pending.push_back(std::move(entry.destroy));
                          return true;
                      });
    }
    for (auto& release : pending)
        release();
}

void DeferredRelease::RetireOwner(uint64_t owner, bool destroy) { FinishOwner(owner, destroy, true); }

void DeferredRelease::FlushOwner(uint64_t owner) { FinishOwner(owner, true, false); }

static void ReleaseReady(bool present)
{
    std::lock_guard executionLock(executionMutex);
    if (State::Instance().isShuttingDown)
        return;

    std::vector<std::function<void()>> ready;

    {
        std::lock_guard lock(mutex);

        if (present)
            presentCounter++;

        if (entries.empty())
            return;

        const auto now = std::chrono::steady_clock::now();

        std::erase_if(entries,
                      [&](Entry& entry)
                      {
                          const auto age = now - entry.time;
                          const bool presentsPassed = presentCounter - entry.present >= MinPresents && age >= MinAge;

                          // Presents are only counted when the swapchain is hooked
                          if (!presentsPassed && age < FallbackAge)
                              return false;

                          ready.push_back(std::move(entry.destroy));
                          return true;
                      });
    }

    for (auto& destroy : ready)
        destroy();

    if (!ready.empty())
        LOG_DEBUG("Released {} deferred objects", ready.size());
}

void DeferredRelease::Process() { ReleaseReady(true); }

void DeferredRelease::Collect() { ReleaseReady(false); }
