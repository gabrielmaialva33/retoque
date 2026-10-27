#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace DlssNr::VkRecording
{
using Handle = uint64_t;
struct Ticket
{
    Handle device = 0;
    Handle buffer = 0;
    uint64_t epoch = 0;
};

// This registry observes successful Vulkan lifecycle calls; it never issues them. A recording keeps its pins
// through End, submission and completion, because an executable command buffer may be submitted again.
// The caller must stop admissions and establish device safety before ForgetDevice; tokens themselves must not
// issue Vulkan calls on a lost device. As with the Vulkan API, callers must externally synchronize each command
// buffer's lifecycle. The registry mutex additionally protects unrelated buffers recorded on different threads.
class Registry
{
    // A record owns one batch, whose next is null. The list is constructed only inside Pin's allocation guard.
    // Detached batches use next solely for an allocation-free retirement chain.
    struct PinBatch
    {
        std::list<std::shared_ptr<void>> owners;
        std::unique_ptr<PinBatch> next;
    };

    struct RetiredPins
    {
        std::unique_ptr<PinBatch> head;

        void Take(std::unique_ptr<PinBatch> batch) noexcept
        {
            if (batch == nullptr)
                return;
            batch->next = std::move(head);
            head = std::move(batch);
        }

        ~RetiredPins()
        {
            // Detach next before destroying each batch: retiring many records must not recurse through the chain.
            while (head != nullptr)
            {
                auto batch = std::move(head);
                head = std::move(batch->next);
            }
        }
    };

    struct Record
    {
        Handle device = 0;
        Handle pool = 0;
        uint64_t epoch = 0;
        bool recording = false;
        std::unique_ptr<PinBatch> pins;
    };

    using Records = std::unordered_map<Handle, Record>;
    mutable std::mutex _mutex;
    // Some standard libraries allocate even for an empty map. The first callback may arrive under OOM, so only
    // Allocate creates the map, inside its allocation guard; unknown callbacks remain inadmissible.
    std::optional<Records> _records;
    uint64_t _nextEpoch = 1;
    bool _closing = false;

    template <class Predicate> void RetireMatching(Predicate matches, bool erase)
    {
        RetiredPins retired;
        {
            std::lock_guard lock(_mutex);
            if (_closing || !_records)
                return;
            for (auto it = _records->begin(); it != _records->end();)
            {
                if (!matches(it->second))
                {
                    ++it;
                    continue;
                }
                retired.Take(std::move(it->second.pins));
                it->second.recording = false;
                if (erase)
                    it = _records->erase(it);
                else
                    ++it;
            }
        }
        // No user-controlled deleter executes while the registry mutex is held.
    }

  public:
    ~Registry()
    {
        RetiredPins retired;
        {
            std::lock_guard lock(_mutex);
            _closing = true;
            if (_records)
            {
                for (auto& [buffer, record] : *_records)
                    retired.Take(std::move(record.pins));
                _records->clear();
            }
        }
    }

    void Allocate(Handle device, Handle pool, const std::vector<Handle>& buffers)
    {
        if (device == 0 || pool == 0 || buffers.empty())
            return;
        RetiredPins retired;
        try
        {
            std::lock_guard lock(_mutex);
            if (_closing)
                return;
            if (!_records)
                _records.emplace();
            for (Handle buffer : buffers)
            {
                if (buffer == 0)
                    continue;
                auto& record = _records->try_emplace(buffer).first->second;
                retired.Take(std::move(record.pins));
                record.device = device;
                record.pool = pool;
                record.epoch = 0;
                record.recording = false;
            }
        }
        catch (const std::bad_alloc&)
        {
            // Unregistered buffers remain inadmissible; never invent a recording after allocation failure.
        }
    }

    void Begin(Handle buffer)
    {
        RetiredPins retired;
        {
            std::lock_guard lock(_mutex);
            if (_closing || !_records)
                return;
            const auto it = _records->find(buffer);
            if (it == _records->end())
                return;
            auto& record = it->second;
            retired.Take(std::move(record.pins));
            record.recording = _nextEpoch != 0;
            record.epoch = _nextEpoch;
            // After the last representable epoch, refuse new recordings instead of reusing old ticket values.
            if (_nextEpoch != 0)
                ++_nextEpoch;
        }
    }

    void End(Handle buffer)
    {
        std::lock_guard lock(_mutex);
        if (_closing || !_records)
            return;
        const auto it = _records->find(buffer);
        if (it != _records->end())
            it->second.recording = false;
    }

    void Reset(Handle buffer)
    {
        RetiredPins retired;
        {
            std::lock_guard lock(_mutex);
            if (_closing || !_records)
                return;
            const auto it = _records->find(buffer);
            if (it == _records->end())
                return;
            retired.Take(std::move(it->second.pins));
            it->second.recording = false;
        }
    }

    void ResetPool(Handle device, Handle pool)
    {
        RetireMatching([=](const Record& record) { return record.device == device && record.pool == pool; }, false);
    }

    // Retirement must not allocate: a freed native handle can immediately be recycled by a different pool.
    // Extract the existing map node, then destroy its pins outside the registry mutex.
    void Free(Handle device, Handle pool, Handle buffer)
    {
        Records::node_type retired;
        {
            std::lock_guard lock(_mutex);
            if (_closing || !_records)
                return;
            const auto it = _records->find(buffer);
            if (it == _records->end() || it->second.device != device || it->second.pool != pool)
                return;
            retired = _records->extract(it);
        }
    }

    // Detach the whole native free batch before any deleter runs. A callback may recycle another handle from
    // the same batch, so a per-buffer retirement loop could otherwise erase that new allocation's record.
    // bufferAt is a pure, nonthrowing array lookup; it avoids allocating a converted Vulkan-handle vector.
    template <class BufferAt> void Free(Handle device, Handle pool, std::size_t count, BufferAt bufferAt)
    {
        static_assert(noexcept(bufferAt(std::size_t {})), "Free's handle lookup must not throw");
        RetiredPins retired;
        {
            std::lock_guard lock(_mutex);
            if (_closing || !_records)
                return;
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto it = _records->find(bufferAt(i));
                if (it == _records->end() || it->second.device != device || it->second.pool != pool)
                    continue;
                retired.Take(std::move(it->second.pins));
                _records->erase(it);
            }
        }
    }

    void Free(Handle device, Handle pool, const std::vector<Handle>& buffers)
    {
        Free(device, pool, buffers.size(), [&buffers](std::size_t i) noexcept { return buffers[i]; });
    }

    void DestroyPool(Handle device, Handle pool)
    {
        RetireMatching([=](const Record& record) { return record.device == device && record.pool == pool; }, true);
    }

    void ForgetDevice(Handle device)
    {
        RetireMatching([=](const Record& record) { return record.device == device; }, true);
    }

    std::optional<Ticket> Current(Handle device, Handle buffer) const
    {
        std::lock_guard lock(_mutex);
        if (_closing || !_records)
            return std::nullopt;
        const auto it = _records->find(buffer);
        if (it == _records->end() || it->second.device != device || !it->second.recording)
            return std::nullopt;
        return Ticket { device, buffer, it->second.epoch };
    }

    bool Pin(const Ticket& ticket, const std::vector<std::shared_ptr<void>>& pins)
    {
        std::unique_ptr<PinBatch> incoming;
        try
        {
            for (const auto& pin : pins)
            {
                if (pin.use_count() == 0)
                    continue;
                if (incoming == nullptr)
                    incoming = std::make_unique<PinBatch>();
                const bool duplicate =
                    std::any_of(incoming->owners.begin(), incoming->owners.end(), [&](const auto& existing)
                                { return !existing.owner_before(pin) && !pin.owner_before(existing); });
                if (!duplicate)
                    incoming->owners.push_back(pin);
            }
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }

        {
            std::lock_guard lock(_mutex);
            if (_closing || !_records)
                return false;
            const auto it = _records->find(ticket.buffer);
            if (it == _records->end() || it->second.device != ticket.device || it->second.epoch != ticket.epoch ||
                !it->second.recording)
                return false;

            if (incoming == nullptr)
                return true;
            auto& held = it->second.pins;
            if (held == nullptr)
            {
                held = std::move(incoming);
                return true;
            }
            for (auto next = incoming->owners.begin(); next != incoming->owners.end();)
            {
                auto pin = next++;
                const bool duplicate =
                    std::any_of(held->owners.begin(), held->owners.end(), [&](const auto& existing)
                                { return !existing.owner_before(*pin) && !pin->owner_before(existing); });
                if (!duplicate)
                    held->owners.splice(held->owners.end(), incoming->owners, pin);
            }
            // All possible allocation failures preceded mutation. Duplicates stay in incoming and are destroyed
            // after releasing the mutex; a stale ticket retains none, and a valid ticket retains every owner once.
        }
        return true;
    }
};

inline Registry& Recordings()
{
    static Registry registry;
    return registry;
}

class SlotPool
{
    struct State
    {
        std::mutex mutex;
        std::vector<bool> used;
        uint32_t freeCount;

        explicit State(uint32_t count) : used(count, false), freeCount(count) {}
    };

    std::shared_ptr<State> _state;

  public:
    class Lease
    {
        friend class SlotPool;
        std::shared_ptr<State> _state;
        std::vector<uint32_t> _slots;

        Lease(std::shared_ptr<State> state, uint32_t count) : _state(std::move(state)), slots(_slots)
        {
            _slots.reserve(count);
        }

      public:
        // A lease's membership cannot be changed by its caller; copies of the shared_ptr share the same lease.
        const std::vector<uint32_t>& slots;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        ~Lease()
        {
            if (_slots.empty())
                return;
            std::lock_guard lock(_state->mutex);
            for (uint32_t slot : _slots)
                _state->used[slot] = false;
            _state->freeCount += static_cast<uint32_t>(_slots.size());
        }
    };

    explicit SlotPool(uint32_t count) : _state(std::make_shared<State>(count)) {}
    SlotPool(const SlotPool&) = delete;
    SlotPool& operator=(const SlotPool&) = delete;

    std::shared_ptr<Lease> Acquire(uint32_t count)
    {
        if (count == 0 || count > _state->used.size())
            return {};
        std::shared_ptr<Lease> lease;
        try
        {
            // Allocate the owner and all slot-index capacity before locking or marking any slot busy.
            lease = std::shared_ptr<Lease>(new Lease(_state, count));
        }
        catch (const std::bad_alloc&)
        {
            return {};
        }
        {
            std::lock_guard lock(_state->mutex);
            if (count > _state->freeCount)
                return {};
            for (uint32_t slot = 0; slot < _state->used.size() && lease->_slots.size() < count; ++slot)
            {
                if (_state->used[slot])
                    continue;
                lease->_slots.push_back(slot);
                _state->used[slot] = true;
            }
            _state->freeCount -= count;
        }
        return lease;
    }
};
} // namespace DlssNr::VkRecording
