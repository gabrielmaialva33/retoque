#include <dlssnr/DlssNr_VkRecording.h>

#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace DlssNr::VkRecording;

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

Ticket Recording(Registry& registry, Handle device, Handle buffer)
{
    auto ticket = registry.Current(device, buffer);
    Check(ticket.has_value(), "known open recording must have a ticket");
    return *ticket;
}

void KnownRecordingAndFailedNotifications()
{
    Registry registry;
    registry.Begin(11);
    Check(!registry.Current(1, 11), "unknown allocation must remain inadmissible");
    registry.Allocate(1, 5, { 11 });
    Check(!registry.Current(1, 11), "allocation alone must not admit recording");
    registry.Begin(11);
    const auto first = Recording(registry, 1, 11);
    Check(first.device == 1 && first.buffer == 11, "ticket must identify the admitted buffer and device");
    Check(!registry.Current(2, 11), "wrong device must not acquire a ticket");

    auto resource = std::make_shared<int>(7);
    std::weak_ptr<int> observed = resource;
    Check(registry.Pin(first, { resource }), "valid recording must accept resource pins");
    resource.reset();
    // A failed Vulkan begin/reset is deliberately NOT reported to this helper.
    const auto unchanged = Recording(registry, 1, 11);
    Check(unchanged.epoch == first.epoch && !observed.expired(), "unreported failed operation must preserve pins");
    registry.Reset(11);
    Check(observed.expired(), "successful reset must release the recorded resource");
}

void EndAndResubmissionKeepResources()
{
    Registry registry;
    registry.Allocate(1, 5, { 11 });
    registry.Begin(11);
    auto ticket = Recording(registry, 1, 11);
    auto resource = std::make_shared<int>(7);
    std::weak_ptr<int> observed = resource;
    Check(registry.Pin(ticket, { resource }), "pin must succeed before End");
    resource.reset();
    registry.End(11);
    Check(!registry.Current(1, 11), "closed recording must reject new work");
    Check(!registry.Pin(ticket, { std::make_shared<int>(8) }), "ticket must not pin after End");
    Check(!observed.expired(), "End must retain resources for submission and legal resubmission");
    // Submission/completion are intentionally not retirement notifications.
    registry.End(11);
    Check(!observed.expired(), "reusable executable recording must retain its resource");
    registry.Begin(11);
    Check(observed.expired(), "successful Begin must retire the previous recording");
    Check(!registry.Pin(ticket, { std::make_shared<int>(9) }), "new Begin must reject the previous ticket");
}

void PinIsAtomicAndDeduplicated()
{
    Registry registry;
    registry.Allocate(1, 5, { 11 });
    registry.Begin(11);
    auto old = Recording(registry, 1, 11);
    registry.Reset(11);
    registry.Begin(11);
    auto first = std::make_shared<int>(1);
    auto second = std::make_shared<int>(2);
    Check(!registry.Pin(old, { first, second }), "stale batch must be rejected");
    Check(first.use_count() == 1 && second.use_count() == 1, "rejected batch must retain no partial pins");
    auto now = Recording(registry, 1, 11);
    Check(registry.Pin(now, { first, first, second }), "current batch must be accepted");
    Check(registry.Pin(now, { first }), "repeated ownership pin must be accepted");
    Check(first.use_count() == 2 && second.use_count() == 2, "one recording must retain each owner once");
    registry.Reset(11);
    Check(first.use_count() == 1 && second.use_count() == 1, "reset must release the whole accepted batch");
}

void PoolAndDeviceIsolation()
{
    Registry registry;
    registry.Allocate(1, 5, { 11, 12 });
    registry.Allocate(1, 6, { 13 });
    registry.Allocate(2, 5, { 21 });
    std::vector<std::weak_ptr<int>> observed;
    for (Handle buffer : { 11, 12, 13, 21 })
    {
        const Handle device = buffer == 21 ? 2 : 1;
        registry.Begin(buffer);
        auto resource = std::make_shared<int>(1);
        observed.emplace_back(resource);
        Check(registry.Pin(Recording(registry, device, buffer), { resource }), "setup pin must succeed");
    }
    registry.ResetPool(1, 5);
    Check(observed[0].expired() && observed[1].expired(), "pool reset must release every member recording");
    Check(!observed[2].expired() && !observed[3].expired(), "pool reset must preserve other pool/device");
    registry.Begin(11);
    Check(registry.Current(1, 11).has_value(), "pool reset must preserve allocations for the next Begin");
    registry.Free(2, 6, { 13 });
    Check(!observed[2].expired(), "wrong-device Free must not retire another device's buffer");
    registry.Free(1, 5, { 13 });
    Check(!observed[2].expired(), "wrong-pool Free must not retire another pool's buffer");
    registry.Free(1, 6, { 13 });
    Check(observed[2].expired(), "Free must retire its matching buffer");
    registry.Begin(13);
    Check(!registry.Current(1, 13), "freed buffer must stay unknown until reallocated");
    registry.DestroyPool(2, 5);
    Check(observed[3].expired(), "DestroyPool must retire its device's matching pool");
    registry.Begin(21);
    Check(!registry.Current(2, 21), "destroyed pool must remove allocation records");
}

void HandleReuseAndDeviceForget()
{
    Registry registry;
    registry.Allocate(1, 5, { 11 });
    registry.Begin(11);
    const auto old = Recording(registry, 1, 11);
    auto resource = std::make_shared<int>(1);
    std::weak_ptr<int> observed = resource;
    Check(registry.Pin(old, { resource }), "setup pin must succeed");
    resource.reset();
    registry.ForgetDevice(1);
    Check(observed.expired() && !registry.Current(1, 11), "forget must drop all records for its device");
    registry.Allocate(1, 5, { 11 });
    registry.Begin(11);
    auto replacement = Recording(registry, 1, 11);
    Check(replacement.epoch != old.epoch, "recycled device/buffer handles must get a fresh epoch");
    Check(!registry.Pin(old, { std::make_shared<int>(3) }), "device forget must invalidate old tickets permanently");
    registry.Free(1, 5, { 11 });
    registry.Allocate(2, 8, { 11 });
    registry.Begin(11);
    Check(!registry.Pin(replacement, { std::make_shared<int>(4) }),
          "cross-device handle reuse must reject old tickets");
    Check(registry.Current(2, 11).has_value(), "reused handle must serve its new device");
}

void QuiescenceAndPartialRecording()
{
    Registry registry;
    registry.Allocate(1, 5, { 11, 12 });
    auto generation = std::make_shared<int>(1);
    for (Handle buffer : { 11, 12 })
    {
        registry.Begin(buffer);
        Check(registry.Pin(Recording(registry, 1, buffer), { generation }), "generation must pin before recording");
        registry.End(buffer);
    }
    Check(generation.use_count() == 3, "two recordings must retain the active generation");
    // Resize admission refuses to acquire new pins while previous recordings remain.
    registry.Reset(11);
    registry.Begin(11);
    Check(generation.use_count() == 2, "one recycled buffer must not declare generation quiescent");
    registry.Reset(12);
    Check(generation.use_count() == 1, "quiescence must converge when refused frames add no pins");

    Check(registry.Pin(Recording(registry, 1, 11), { generation }), "partial recording must establish its pin");
    // Simulate a failure after commands were emitted: no reset/free notification is legal here.
    Check(generation.use_count() == 2, "partial recording failure must not silently retire its pin");
    registry.Reset(11);
    Check(generation.use_count() == 1, "actual reset must retire the failed partial recording");
}

struct Reenter
{
    std::function<void()> onRelease;
    ~Reenter() { onRelease(); }
};

void RetirementRunsOutsideRegistryLock()
{
    for (int operation = 0; operation != 7; ++operation)
    {
        Registry registry;
        registry.Allocate(1, 5, { 11 });
        registry.Begin(11);
        int released = 0;
        bool reentered = false;
        auto token = std::make_shared<Reenter>();
        token->onRelease = [&]
        {
            ++released;
            registry.Allocate(2, 8, { 21 });
            registry.Begin(21);
            reentered = registry.Current(2, 21).has_value();
        };
        Check(registry.Pin(Recording(registry, 1, 11), { token }), "reentrant token pin must succeed");
        token.reset();
        switch (operation)
        {
        case 0:
            registry.Begin(11);
            break;
        case 1:
            registry.Reset(11);
            break;
        case 2:
            registry.ResetPool(1, 5);
            break;
        case 3:
            registry.Free(1, 5, { 11 });
            break;
        case 4:
            registry.DestroyPool(1, 5);
            break;
        case 5:
            registry.ForgetDevice(1);
            break;
        case 6:
            registry.Allocate(1, 6, { 11 });
            break;
        }
        Check(released == 1, "retirement must execute a token's release exactly once");
        Check(reentered, "retirement callback must be able to reenter registry");
    }
}

void BulkFreeDetachesBeforeReentry()
{
    Registry registry;
    registry.Allocate(1, 5, { 11, 12 });
    registry.Begin(11);
    registry.Begin(12);
    bool reentered = false;
    bool allDetached = false;
    uint64_t replacementEpoch = 0;
    std::weak_ptr<int> replacement;
    auto first = std::make_shared<Reenter>();
    first->onRelease = [&]
    {
        allDetached = !registry.Current(1, 11) && !registry.Current(1, 12);
        // Both native buffers were already freed. A release callback may allocate and record the recycled handle.
        registry.Allocate(1, 5, { 12 });
        registry.Begin(12);
        const auto ticket = registry.Current(1, 12);
        auto token = std::make_shared<int>(3);
        replacement = token;
        reentered = ticket.has_value() && registry.Pin(*ticket, { token });
        replacementEpoch = ticket ? ticket->epoch : 0;
    };
    Check(registry.Pin(Recording(registry, 1, 11), { first }), "setup must pin the first bulk member");
    first.reset();
    auto second = std::make_shared<int>(2);
    std::weak_ptr<int> originalSecond = second;
    Check(registry.Pin(Recording(registry, 1, 12), { second }), "setup must pin the second bulk member");
    second.reset();
    const std::vector<Handle> buffers { 11, 12 };
    registry.Free(1, 5, buffers);
    const auto current = registry.Current(1, 12);
    Check(reentered && current && current->epoch == replacementEpoch && !replacement.expired(),
          "bulk Free must preserve a new allocation created by an earlier member's release callback");
    Check(allDetached && originalSecond.expired(),
          "bulk Free must detach every old member before any release callback");
    registry.Reset(12);
    Check(replacement.expired(), "new allocation must retire only at its own later lifecycle operation");
}

void SlotsNeverWrapIntoLiveReservations()
{
    SlotPool pool(18);
    auto a = pool.Acquire(6);
    auto b = pool.Acquire(6);
    auto c = pool.Acquire(6);
    Check(a && b && c, "three six-dispatch frames must fit the eighteen-slot budget");
    std::vector<bool> occupied(18, false);
    for (const auto& lease : { a, b, c })
        for (auto slot : lease->slots)
        {
            Check(slot < 18 && !occupied[slot], "live reservations must never share a slot");
            occupied[slot] = true;
        }
    Check(!pool.Acquire(1), "nineteenth live slot must be refused instead of wrapping");
    const auto releasedSlots = b->slots;
    b.reset();
    Check(!pool.Acquire(7), "oversized batch must fail atomically with six slots free");
    b = pool.Acquire(6);
    Check(b && b->slots == releasedSlots, "failed reservation must leave released slots available");
    Check(!pool.Acquire(1), "reacquired capacity must again be exhausted");
}

void LeaseStateOutlivesPoolAndCopies()
{
    std::shared_ptr<SlotPool::Lease> lease;
    {
        SlotPool pool(2);
        lease = pool.Acquire(2);
        Check(lease && lease->slots.size() == 2, "full pool reservation must succeed");
        auto copy = lease;
        lease.reset();
        Check(!pool.Acquire(1), "copy of a lease must continue to own its slots");
        lease = std::move(copy);
    }
    Check(lease->slots.size() == 2, "lease must keep its state alive after pool object destruction");
    lease.reset(); // ASan verifies that release does not touch the destroyed SlotPool.
}

void ReservationFollowsRecordingLifetime()
{
    Registry registry;
    SlotPool pool(18);
    registry.Allocate(1, 5, { 11 });
    registry.Begin(11);
    auto reservation = pool.Acquire(18);
    Check(reservation != nullptr, "setup must reserve the complete frame budget");
    Check(registry.Pin(Recording(registry, 1, 11), { reservation }), "recording must own its slot lease");
    reservation.reset();
    registry.End(11);
    Check(!pool.Acquire(1), "ending a recording must not recycle its slots");
    registry.Reset(11);
    Check(pool.Acquire(18) != nullptr, "reset must return all slots to the independent pool state");
}

void ConcurrentReservationsAreDisjoint()
{
    SlotPool pool(4);
    std::atomic<int> occupancy[4] {};
    std::atomic<bool> failed = false;
    std::vector<std::thread> threads;
    for (int thread = 0; thread != 8; ++thread)
        threads.emplace_back(
            [&]
            {
                for (int attempt = 0; attempt != 1000; ++attempt)
                {
                    auto lease = pool.Acquire(1);
                    if (!lease)
                        continue;
                    const auto slot = lease->slots.front();
                    if (occupancy[slot].fetch_add(1) != 0)
                        failed.store(true);
                    std::this_thread::yield();
                    if (occupancy[slot].fetch_sub(1) != 1)
                        failed.store(true);
                }
            });
    for (auto& thread : threads)
        thread.join();
    Check(!failed.load(), "parallel reservations must never use the same live slot");
    Check(pool.Acquire(4) != nullptr, "parallel reservations must return every slot");
}
} // namespace

int main()
{
    const std::pair<const char*, void (*)()> cases[] = {
        { "known-recording-and-failed-notifications", KnownRecordingAndFailedNotifications },
        { "end-and-resubmission", EndAndResubmissionKeepResources },
        { "atomic-pins", PinIsAtomicAndDeduplicated },
        { "pool-device-isolation", PoolAndDeviceIsolation },
        { "handle-reuse", HandleReuseAndDeviceForget },
        { "quiescence-partial-recording", QuiescenceAndPartialRecording },
        { "reentrant-retirement", RetirementRunsOutsideRegistryLock },
        { "bulk-free-reentrant-reuse", BulkFreeDetachesBeforeReentry },
        { "slot-wrap-and-atomic-reservation", SlotsNeverWrapIntoLiveReservations },
        { "lease-lifetime", LeaseStateOutlivesPoolAndCopies },
        { "recording-slot-lifetime", ReservationFollowsRecordingLifetime },
        { "concurrent-reservations", ConcurrentReservationsAreDisjoint },
    };
    int failures = 0;
    for (const auto& [name, run] : cases)
    {
        try
        {
            run();
            std::cout << "PASS: " << name << '\n';
        }
        catch (const std::exception& error)
        {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
