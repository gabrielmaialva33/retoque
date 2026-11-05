#pragma once

#include <atomic>

// The client owns Vulkan binding state across the complete NGX evaluate window (DLSS guide 5.2.5).
// Only the Color parameter is borrowed. Never replay pre-NR bindings over the provider's final state.
namespace DlssNr::VkPre
{
// Serializes the first placement choice, including a cold post call racing a cold pre call. Post-only sessions
// keep their existing recording behavior. After Pre claims the process, Post must borrow the same CPU loan.
// Deliberately never reset: a cold Post caller may already be waiting for the model lock during cleanup.
class Placement
{
    enum : unsigned
    {
        Unclaimed,
        Post,
        Pre
    };
    std::atomic<unsigned> _mode { Unclaimed };

  public:
    bool TryPre()
    {
        unsigned expected = Unclaimed;
        _mode.compare_exchange_strong(expected, Pre, std::memory_order_acq_rel);
        return expected != Post;
    }
    bool PostNeedsLoan()
    {
        unsigned expected = Unclaimed;
        _mode.compare_exchange_strong(expected, Post, std::memory_order_acq_rel);
        return expected == Pre;
    }
};

template <class Params> class ColourLoan
{
    Params* _params = nullptr;
    void* _original = nullptr;

  public:
    ColourLoan() = default;
    ColourLoan(const ColourLoan&) = delete;
    ColourLoan& operator=(const ColourLoan&) = delete;
    ~ColourLoan() { Restore(); }
    void Bind(Params* params, void* original, void* scratch)
    {
        _params = params;
        _original = original;
        _params->Set("Color", scratch);
    }
    void Restore() noexcept
    {
        if (_params)
        {
            auto* params = _params;
            _params = nullptr;
            params->Set("Color", _original);
        }
    }
};
} // namespace DlssNr::VkPre
