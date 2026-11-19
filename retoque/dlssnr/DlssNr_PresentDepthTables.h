#pragma once
#include "DlssNr_PresentDepth.h"
#include <atomic>

namespace DlssNr::PresentDepth
{
struct View
{
    Id id = 0;
    uint64_t flags = 0;
};
// Writers hold Owner::mutex. Hot recording threads use bounded seqlock reads, never wait on that lock.
// Keys are not reclaimed; capacity exhaustion is an explicit unobserved interval, not unsafe eviction.
template <size_t N> struct AddressTable
{
    static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<uintptr_t>::is_always_lock_free);
    struct Slot
    {
        std::atomic<uintptr_t> key { 0 };
        std::atomic<uint64_t> version { 0 }, id { 0 }, flags { 0 };
    };
    std::array<Slot, N> slots {};
    Slot* Find(uintptr_t key, bool insert)
    {
        if (!key)
            return nullptr;
        const auto first = (key >> 4) % N;
        for (size_t n = 0; n < N; ++n)
        {
            auto& s = slots[(first + n) % N];
            const auto found = s.key.load(std::memory_order_seq_cst);
            if (found == key)
                return &s;
            if (!found)
            {
                if (!insert)
                    return nullptr;
                s.key.store(key, std::memory_order_seq_cst);
                return &s;
            }
        }
        return nullptr;
    }
    bool Put(uintptr_t key, View value)
    {
        auto* s = Find(key, true);
        if (!s)
            return false;
        s->version.fetch_add(1, std::memory_order_seq_cst);
        s->id.store(value.id, std::memory_order_seq_cst);
        s->flags.store(value.flags, std::memory_order_seq_cst);
        s->version.fetch_add(1, std::memory_order_seq_cst);
        return true;
    }
    View Get(uintptr_t key)
    {
        auto* s = Find(key, false);
        if (!s)
            return {};
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const auto before = s->version.load(std::memory_order_seq_cst);
            if (before & 1)
                continue;
            View v { s->id.load(std::memory_order_seq_cst), s->flags.load(std::memory_order_seq_cst) };
            if (before == s->version.load(std::memory_order_seq_cst))
                return v;
        }
        return {};
    }
};
} // namespace DlssNr::PresentDepth
