#pragma once

#include "DlssNr_VkChain.h"
#include "DlssNr_VkBudget.h"
#include "DlssNr_VkQueueProof.h"
#include <vulkan/vulkan.h>
#include <array>
#include <atomic>
#include <mutex>
#include <type_traits>

namespace DlssNr::VkChain
{
struct CompletionFunctions
{
    PFN_vkCreateFence create = nullptr;
    PFN_vkDestroyFence destroy = nullptr;
    PFN_vkGetFenceStatus status = nullptr;
    PFN_vkQueueSubmit marker = nullptr; // original legacy trampoline, never the public hook
    bool Valid() const { return create && destroy && status && marker; }
};

// One logical device, at most ten creation recordings/fences. Only CPU metadata is pinned in
// the recording registry. This owns no NGX/image resource and never authorizes their release.
// Queue serialization is the caller's. No registry mutex is held across a driver call.
// Lock order: existing queue or NR lock -> this observer -> registry (Watch only).
// The driver trampolines must not enter NR; nested submit hooks bypass this observer.
template <class Mutex = std::mutex> class CreationObserver
{
    struct Life
    {
        std::atomic<bool> retired { false };
    };
    struct Retirement
    {
        std::shared_ptr<Life> life;
        explicit Retirement(std::shared_ptr<Life> value) : life(std::move(value)) {}
        ~Retirement() { life->retired.store(true, std::memory_order_release); }
    };
    struct Entry
    {
        uint64_t serial = 0, offEpoch = 0;
        VkRecording::Ticket ticket {};
        std::optional<Creation> proof;
        std::shared_ptr<Life> life;
        VkFence fence = VK_NULL_HANDLE;
        bool seen = false, invalid = false;
    };
    std::atomic<VkDevice> _device { VK_NULL_HANDLE };
    std::atomic<bool> _active { false }, _lost { false }, _overflow { false };
    std::atomic<uint64_t> _offEpoch { 0 };
    inline static std::atomic<uint64_t> _nextSerial { 1 }; // shared across slots and device-handle reuse
    CompletionFunctions _fn;
    BudgetSource _budget;
    QueueProof<Mutex> _order;
    Mutex _mutex;
    std::array<Entry, 10> _entries {};

    bool Retired(const Entry& e) const { return e.life && e.life->retired.load(std::memory_order_acquire); }
    void Invalidate(Entry& e)
    {
        e.invalid = true;
        if (e.proof)
            e.proof->Invalidate();
    }
    void PollLocked()
    {
        for (auto& e : _entries)
        {
            if (!e.proof)
                continue;
            if (Retired(e) || e.offEpoch != _offEpoch.load(std::memory_order_acquire))
                Invalidate(e);
            if (_lost.load(std::memory_order_acquire) || !e.fence)
                continue;
            const VkResult result = _fn.status(Device(), e.fence);
            if (result == VK_ERROR_DEVICE_LOST)
                Lost();
            else if (result == VK_SUCCESS && !_lost.load(std::memory_order_acquire))
            {
                if (!e.invalid)
                    e.proof->Complete(e.ticket, e.serial);
                // Only a private fence observed signalled (or a device-wide live drain at Destroy)
                // can be destroyed. A failed marker submit retains its fence, never guesses.
                _fn.destroy(Device(), e.fence, nullptr);
                e.fence = VK_NULL_HANDLE;
            }
            else if (result != VK_NOT_READY)
                Invalidate(e);
        }
    }

  public:
    void ConfigureBudget(VkPhysicalDevice physical, bool enabledAndSupported,
                         PFN_vkGetPhysicalDeviceMemoryProperties2 query)
    {
        _budget.Configure(physical, enabledAndSupported, query, BudgetNowMs, &_lost);
    }
    QueueProof<Mutex>& Order() { return _order; }
    const BudgetSource& Budget() const { return _budget; }
    VkDevice Device() const { return _device.load(std::memory_order_acquire); }
    // Before the device is exposed to application callbacks; serialized by the existing install lock.
    bool Initialize(VkDevice device, CompletionFunctions functions)
    {
        if (Device() || !device || !functions.Valid())
            return false;
        _fn = functions;
        _budget = {};
        _order.Configure({});
        _lost.store(false, std::memory_order_relaxed);
        _offEpoch.store(0, std::memory_order_relaxed);
        _overflow.store(false, std::memory_order_relaxed);
        _active.store(false, std::memory_order_relaxed);
        _device.store(device, std::memory_order_release);
        return true;
    }
    // Called by inactive submit forwarding: no lock, allocation, Vulkan call or change to arguments.
    // Evidence recorded before an unobserved/off interval can never become ready after re-enabling.
    void Off()
    {
        if (_offEpoch.fetch_add(1, std::memory_order_acq_rel) == UINT64_MAX)
            _overflow.store(true, std::memory_order_release);
    }
    void Lost() { _lost.store(true, std::memory_order_release); }

    uint64_t Watch(const VkRecording::Ticket& ticket, uint64_t present, VkRecording::Registry& registry)
    {
        if (ticket.device != (uint64_t) Device() || !present || ticket.level != VkRecording::Level::Primary ||
            ticket.reuse != VkRecording::Reuse::Serial || !ticket.oneTime || _lost.load() || _overflow.load())
            return 0;
        std::lock_guard lock(_mutex);
        const auto current = registry.Current(ticket.device, ticket.buffer);
        if (!current || current->epoch != ticket.epoch || current->level != ticket.level ||
            current->reuse != ticket.reuse || !current->oneTime)
            return 0;
        PollLocked();
        if (_lost.load())
            return 0;
        for (auto& e : _entries)
        {
            if (e.proof && !Retired(e) && e.ticket.buffer == ticket.buffer && e.ticket.epoch == ticket.epoch)
                return 0; // one creation per recording in v1
        }
        for (auto& e : _entries)
        {
            if (e.proof && (!Retired(e) || e.fence))
                continue;
            try
            {
                auto serial = _nextSerial.load(std::memory_order_relaxed);
                while (serial != 0 && !_nextSerial.compare_exchange_weak(serial, serial == UINT64_MAX ? 0 : serial + 1,
                                                                         std::memory_order_relaxed))
                {
                }
                if (!serial)
                    return 0; // exhaustion is permanent, never recycle a completion identity
                auto life = std::make_shared<Life>();
                auto retirement = std::make_shared<Retirement>(life);
                if (!registry.Pin(ticket, { retirement }))
                    return 0;
                e = {};
                e.life = std::move(life);
                e.ticket = ticket;
                e.serial = serial;
                e.offEpoch = _offEpoch.load(std::memory_order_acquire);
                e.proof.emplace(ticket, present);
                _active.store(true, std::memory_order_release);
                return e.serial;
            }
            catch (const std::bad_alloc&)
            {
                return 0; // no driver work was recorded by this observer
            }
        }
        return 0;
    }

    // eachBuffer visits the exact top-level command buffers before call() forwards the application
    // submit. It neither allocates nor invokes Vulkan. The original result is always returned intact.
    template <class EachBuffer, class Call> VkResult Submit(VkQueue queue, EachBuffer eachBuffer, Call call)
    {
        if (!_active.load(std::memory_order_acquire) || _lost.load(std::memory_order_acquire))
            return call();
        std::lock_guard lock(_mutex);
        std::array<unsigned, 10> matches {};
        eachBuffer(
            [&](VkCommandBuffer buffer)
            {
                for (size_t i = 0; i < _entries.size(); ++i)
                {
                    const auto& e = _entries[i];
                    if (e.proof && !Retired(e) && e.ticket.buffer == (uint64_t) buffer)
                        ++matches[i];
                }
            });
        // Snapshot precedes the driver call, so newly armed work cannot inherit an older submit.
        const auto epoch = _offEpoch.load(std::memory_order_acquire);
        const VkResult result = call();
        if (result == VK_ERROR_DEVICE_LOST)
            Lost();
        for (size_t i = 0; i < _entries.size(); ++i)
        {
            if (!matches[i])
                continue;
            auto& e = _entries[i];
            const bool first = !e.seen;
            e.seen = true;
            if (result != VK_SUCCESS || _lost.load() || _overflow.load() || matches[i] != 1 || !first || Retired(e) ||
                e.offEpoch != epoch || epoch != _offEpoch.load(std::memory_order_acquire))
            {
                Invalidate(e);
                continue;
            }
            if (!e.proof->Submitted(e.ticket, e.serial))
            {
                Invalidate(e);
                continue;
            }
            VkFenceCreateInfo info {};
            info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            const auto created = _fn.create(Device(), &info, nullptr, &e.fence);
            if (created == VK_ERROR_DEVICE_LOST)
                Lost();
            if (created != VK_SUCCESS || _lost.load())
            {
                e.fence = VK_NULL_HANDLE; // failed create or lost device: no destructor
                Invalidate(e);
                continue;
            }
            VkSubmitInfo marker {};
            marker.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            const auto marked = _fn.marker(queue, 1, &marker, e.fence);
            if (marked == VK_ERROR_DEVICE_LOST)
                Lost();
            if (marked != VK_SUCCESS || _lost.load())
                Invalidate(e); // retain even after non-loss failure until signal or proven drain
        }
        return result;
    }
    bool Ready(uint64_t serial, uint64_t present)
    {
        if (!serial || _lost.load() || _overflow.load())
            return false;
        std::lock_guard lock(_mutex);
        PollLocked();
        if (_lost.load())
            return false;
        for (auto& e : _entries)
            if (e.serial == serial && e.proof)
                return !e.invalid && !Retired(e) && e.offEpoch == _offEpoch.load() && e.proof->Ready(present);
        return false;
    }
    void Cancel(uint64_t serial)
    {
        std::lock_guard lock(_mutex);
        for (auto& e : _entries)
            if (e.serial == serial)
                Invalidate(e);
    }
    // Only at externally synchronized VkDevice destruction, after the host's drain decision. No
    // additional mutex even if Passes was switched back to one. On unproved/lost devices abandon.
    void Destroy(bool drained)
    {
        for (auto& e : _entries)
        {
            if (e.life)
                e.life->retired.store(true, std::memory_order_release);
            if (drained && !_lost.load() && e.fence)
                _fn.destroy(Device(), e.fence, nullptr);
            e = {};
        }
        _active.store(false, std::memory_order_release);
        _device.store(VK_NULL_HANDLE, std::memory_order_release);
    }
};
} // namespace DlssNr::VkChain
