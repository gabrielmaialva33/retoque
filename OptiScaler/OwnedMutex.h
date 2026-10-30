#pragma once

#include "SysUtils.h"

#include <atomic>
#include <shared_mutex>
#include <thread>

class OwnedMutex
{
  private:
    std::shared_mutex mtx;
    std::atomic<uint32_t> owner { 0 }; // don't use 0
    // The holder's thread. Not recursive: a holder re-entered on its own thread (a window message DXGI delivers
    // inside Present) must not wait for itself (Retoque, after ShyVortex's alt-tab fix, hhkbble/OptiScaler 5181a07a)
    std::atomic<std::thread::id> ownerThread {};

  public:
    void lock(uint32_t _owner)
    {
        mtx.lock();
        ownerThread.store(std::this_thread::get_id());
        owner.store(_owner, std::memory_order_release);
    }

    // Only unlocks if owner matches
    void unlockThis(uint32_t _owner)
    {
        uint32_t current_owner = owner.load(std::memory_order_acquire);

        if (current_owner == 0 || current_owner != _owner)
        {
            LOG_WARN("current_owner: {}, _owner: {}", current_owner, _owner);
            return;
        }

        ownerThread.store(std::thread::id {});
        owner.store(0, std::memory_order_release);
        mtx.unlock();
    }

    uint32_t getOwner() { return owner.load(std::memory_order_seq_cst); }

    // Only the holding thread finds its own id here; any other thread reads another id or none
    bool isHeldByCurrentThread() const { return ownerThread.load() == std::this_thread::get_id(); }
};

class OwnedLockGuard
{
  private:
    OwnedMutex& _mutex;
    uint32_t _owner_id;

  public:
    OwnedLockGuard(OwnedMutex& mutex, uint32_t owner_id) : _mutex(mutex), _owner_id(owner_id)
    {
        _mutex.lock(_owner_id);
    }

    ~OwnedLockGuard() { _mutex.unlockThis(_owner_id); }
};
