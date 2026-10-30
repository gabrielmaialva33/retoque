#pragma once

// Reverse index from a tracked D3D12 resource to the descriptor slots that currently reference it, used by
// ResTrack_dx12 (HeapInfo views/copies/clears, heap teardown, resource destruction callbacks).
//
// It replaces one global mutex plus a vector per resource that was scanned with find_if on every attach and
// remove_if on every detach. Games that copy a tracked render target's descriptor into ring heaps every frame
// build thousands of references per resource, so each copy cost O(references) under a process-wide lock.
// Here each resource's references are a hash map keyed by (heap version, descriptor index): attach, duplicate
// check and detach are O(1), and the resources are spread over independently locked shards.
//
// Semantics are those of the former code (differentially tested in tests/restrack-resource-references):
// - a resource is tracked from Register (its destruction callback is installed) until Take (it was destroyed);
//   an emptied reference set keeps the registration;
// - Attach to an untracked resource adds nothing and returns false (the caller drops the view);
// - a (heap version, index) reference is held at most once per resource; the heap factory runs only for a
//   new reference;
// - Take returns the references in unspecified order. Callers clear each slot independently, so the order
//   is not observable; the former vector order was insertion order and is not preserved.
//
// Locking: every member takes exactly one shard lock and acquires nothing else while holding it, except the
// caller's factory/visitor. Callers may hold a HeapInfo descriptor stripe lock (stripe -> shard), never the
// reverse. A resource and all its references always use the same shard.

#include <ankerl/unordered_dense.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

template <class Resource, class Heap, std::size_t Shards = 64> class ResourceReferenceIndex
{
  public:
    static constexpr std::size_t ShardCount = Shards;
    static_assert(ShardCount > 1 && std::has_single_bit(ShardCount));

    struct SlotKey
    {
        std::uint64_t heapVersion = 0;
        std::uint32_t index = 0;

        bool operator==(const SlotKey&) const = default;
    };

    struct SlotKeyHash
    {
        using is_avalanching = void;

        std::uint64_t operator()(const SlotKey& key) const noexcept
        {
            // murmur3 finalizer over both fields
            std::uint64_t hash = key.heapVersion * UINT64_C(0x9E3779B97F4A7C15) ^ key.index;
            hash ^= hash >> 33;
            hash *= UINT64_C(0xFF51AFD7ED558CCD);
            hash ^= hash >> 33;
            hash *= UINT64_C(0xC4CEB9FE1A85EC53);
            hash ^= hash >> 33;
            return hash;
        }
    };

    using References = ankerl::unordered_dense::map<SlotKey, std::weak_ptr<Heap>, SlotKeyHash>;

    static std::size_t ShardOf(const Resource* resource) noexcept
    {
        // Fibonacci hashing of the address without its alignment bits. The maps inside a shard hash the
        // pointer independently, so taking the top bits here does not cluster their buckets.
        auto key = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(resource)) >> 4;
        key ^= key >> 17;
        key *= UINT64_C(0x9E3779B97F4A7C15);
        return static_cast<std::size_t>(key >> (64 - std::countr_zero(ShardCount)));
    }

    void Reserve(std::size_t resourcesPerShard)
    {
        for (auto& shard : _shards)
        {
            std::scoped_lock lock(shard.mutex);
            shard.resources.reserve(resourcesPerShard);
        }
    }

    bool Contains(Resource* resource) const
    {
        auto& shard = ShardFor(resource);
        std::scoped_lock lock(shard.mutex);
        return shard.resources.contains(resource);
    }

    void Register(Resource* resource)
    {
        auto& shard = ShardFor(resource);
        std::scoped_lock lock(shard.mutex);
        shard.resources.try_emplace(resource);
    }

    // makeHeap() -> std::weak_ptr<Heap>, called only when the reference is new.
    template <class MakeHeap>
    bool Attach(Resource* resource, std::uint64_t heapVersion, std::uint32_t index, MakeHeap&& makeHeap)
    {
        auto& shard = ShardFor(resource);
        std::scoped_lock lock(shard.mutex);
        auto it = shard.resources.find(resource);
        if (it == shard.resources.end())
            return false;

        auto& references = it->second;
        const SlotKey key { heapVersion, index };
        if (references.find(key) == references.end())
            references.emplace(key, makeHeap());

        return true;
    }

    void Detach(Resource* resource, std::uint64_t heapVersion, std::uint32_t index)
    {
        auto& shard = ShardFor(resource);
        std::scoped_lock lock(shard.mutex);
        auto it = shard.resources.find(resource);
        if (it != shard.resources.end())
            it->second.erase(SlotKey { heapVersion, index });
    }

    // Forgets the resource and hands back its references for the caller to clear outside the lock.
    References Take(Resource* resource)
    {
        References taken;
        auto& shard = ShardFor(resource);
        std::scoped_lock lock(shard.mutex);
        if (auto it = shard.resources.find(resource); it != shard.resources.end())
        {
            taken = std::move(it->second);
            shard.resources.erase(it);
        }

        return taken;
    }

    // visit(heapVersion, index, const std::weak_ptr<Heap>&) under the shard lock; false when untracked.
    // Diagnostics and tests only: the visitor must not call back into the index.
    template <class Visit> bool ForEach(Resource* resource, Visit&& visit) const
    {
        auto& shard = ShardFor(resource);
        std::scoped_lock lock(shard.mutex);
        auto it = shard.resources.find(resource);
        if (it == shard.resources.end())
            return false;

        for (const auto& [key, heap] : it->second)
            visit(key.heapVersion, key.index, heap);

        return true;
    }

  private:
    // One cache line per shard lock (x64), so neighbouring shards do not false-share.
    struct alignas(64) Shard
    {
        std::mutex mutex;
        ankerl::unordered_dense::map<Resource*, References> resources;
    };

    Shard& ShardFor(const Resource* resource) const { return _shards[ShardOf(resource)]; }

    mutable Shard _shards[ShardCount];
};
