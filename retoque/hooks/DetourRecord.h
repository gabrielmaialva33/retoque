#pragma once

#include <cstddef>
#include <vector>

// Which detours a hook set really attached, so its unhook detaches exactly those (docs/linux-proton/sl-interposer-
// unhook.md, tested by tests/sl-interposer-unhook).
//
// Detours fails a whole transaction when one DetourDetach names a pointer that was never attached: its trampoline
// check rejects the block, the commit aborts, and nothing is detached. StreamlineHooks::unhookInterposer used to
// detach every export it had looked up, attached or not, so on a second sl.interposer it could leave the first one's
// hooks in place and then overwrite their originals. This records each successful DetourAttach of a committed
// transaction, and detaches newest first only what is recorded.
//
// Standard library only: the Detours calls are passed in, so the host test drives the bookkeeping. Not thread-safe
// on its own; the callers hold g_detoursTransactionMutex around every transaction, as before.
namespace Retoque::Hooks
{
class DetourRecord
{
  public:
    struct Entry
    {
        void** target = nullptr; // the original-function pointer Detours rewrites (o_slInit, ...)
        void* detour = nullptr;  // the hook it was attached with
    };

    // A transaction starts: nothing of it is recorded yet.
    void Begin() { _pending.clear(); }

    // DetourAttach inside the transaction; recorded only when Detours accepted it. Returns Detours' answer.
    template <class AttachFn> long Attach(AttachFn&& attach, void** target, void* detour)
    {
        const long result = static_cast<long>(attach(target, detour));
        if (result == 0)
            _pending.push_back({ target, detour });
        return result;
    }

    // The transaction's commit answered: what it attached is recorded only if it committed.
    void Committed(bool ok)
    {
        if (ok)
            _attached.insert(_attached.end(), _pending.begin(), _pending.end());
        _pending.clear();
    }

    // Inside an unhook transaction: DetourDetach every recorded entry, newest first. Returns how many were asked.
    template <class DetachFn> size_t DetachAll(DetachFn&& detach) const
    {
        for (auto it = _attached.rbegin(); it != _attached.rend(); ++it)
            detach(it->target, it->detour);
        return _attached.size();
    }

    // The unhook's commit answered. On success the record is empty and each original pointer is cleared (Detours put
    // the real function back in it, which belongs to a module about to be replaced); on failure every entry stays,
    // still attached as far as anyone can tell.
    void Detached(bool ok)
    {
        if (!ok)
            return;
        for (const auto& entry : _attached)
            *entry.target = nullptr;
        _attached.clear();
    }

    bool Attached(void* const* target) const
    {
        for (const auto& entry : _attached)
        {
            if (entry.target == target)
                return true;
        }
        return false;
    }

    size_t Size() const { return _attached.size(); }

  private:
    std::vector<Entry> _pending;
    std::vector<Entry> _attached;
};
} // namespace Retoque::Hooks
