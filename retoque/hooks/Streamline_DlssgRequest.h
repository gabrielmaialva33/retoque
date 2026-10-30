#pragma once

#include <atomic>
#include <cstdint>

// docs/dlssnr/design/pause-without-frame-gen.md. Which viewport's slDLSSGSetOptions calls may set the game's DLSS-G
// request (State::dlssgGameRequestsFg) that PauseWithoutFrameGen reads. That signal is one bool: a game driving DLSS-G
// on two viewports, one on and one off, would toggle it between them and pause NR while frames are generated. The first
// viewport whose call Streamline accepted owns the signal; once a second one is accepted the pause stands down for the
// rest of the process. A refused call is no viewport. Idea from kkyleeb21's D18 0.4.1 (3f61eb29); ours is independent.
namespace StreamlineDlssgRequest
{
// One lock-free word: bit 0 a viewport was accepted, bit 1 more than one was, bits 32-63 the first one's handle.
inline constexpr uint64_t SeenBit = 1;
inline constexpr uint64_t MultipleBit = 2;
static_assert(std::atomic<uint64_t>::is_always_lock_free);

// A call from `viewport` that Streamline accepted. True when it may set the signal: the only viewport seen so far.
inline bool Accept(std::atomic<uint64_t>& word, uint32_t viewport)
{
    uint64_t old = word.load(std::memory_order_relaxed);

    while (true)
    {
        if (old & MultipleBit)
            return false;

        if (old & SeenBit)
        {
            if (uint32_t(old >> 32) == viewport)
                return true;

            if (word.compare_exchange_weak(old, old | MultipleBit, std::memory_order_relaxed))
                return false;

            continue;
        }

        if (word.compare_exchange_weak(old, (uint64_t { viewport } << 32) | SeenBit, std::memory_order_relaxed))
            return true;
    }
}

inline bool MultipleViewports(const std::atomic<uint64_t>& word)
{
    return (word.load(std::memory_order_relaxed) & MultipleBit) != 0;
}
} // namespace StreamlineDlssgRequest
