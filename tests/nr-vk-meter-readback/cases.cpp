#include "DlssNr_VkMeterReadback.h"
#include "DlssNr_VkRecording.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>

static int allocationBudget = -1;
[[gnu::noinline]] void* Allocate(std::size_t size)
{
    if (allocationBudget == 0)
        throw std::bad_alloc();
    if (allocationBudget > 0)
        --allocationBudget;
    if (void* result = std::malloc(size ? size : 1))
        return result;
    throw std::bad_alloc();
}
[[gnu::noinline]] void Release(void* value) { std::free(value); }
void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void operator delete(void* value) noexcept { Release(value); }
void operator delete[](void* value) noexcept { Release(value); }
void operator delete(void* value, std::size_t) noexcept { Release(value); }
void operator delete[](void* value, std::size_t) noexcept { Release(value); }

using DlssNr::VkMeterReadback::Reservation;
using DlssNr::VkMeterReadback::Ring;
using DlssNr::VkMeterReadback::Sample;
void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
Reservation Reserve(Ring& ring, uint32_t mask = 15)
{
    auto result = ring.Reserve(mask);
    Check(result.has_value(), "expected reservation");
    return std::move(*result);
}
void RegistryLifetime()
{
    Ring ring;
    DlssNr::VkRecording::Registry registry;
    registry.Allocate(1, 2, { 3 });
    registry.Begin(3);
    auto ticket = registry.Current(1, 3);
    Check(ticket.has_value(), "registry begin did not admit recording");
    auto reservation = Reserve(ring, 1);
    Check(registry.Pin(*ticket, { reservation.pin }), "registry pin failed");
    reservation.pin.reset();
    Check(!ring.TakeRetired(0), "readback retired during recording");
    registry.End(3);
    Check(!ring.TakeRetired(0), "End made executable command buffer's readback readable");
    // Successful submission, GPU completion, four later frame attempts and resubmission
    // do not retire an executable command buffer; the actual registry retains its pin.
    for (unsigned frame = 0; frame < 12; ++frame)
    {
        Check(!ring.TakeRetired(0), "frame age/completion exposed a resubmittable readback");
        Check(!ring.Reserve(1), "live executable recording slot was reused");
    }
    registry.Begin(3); // The successful re-begin retires the old recording's pins.
    auto sample = ring.TakeRetired(0);
    Check(sample && sample->usable && sample->serial == reservation.serial,
          "registry re-begin did not retire original sample");
    Check(!ring.TakeRetired(0), "retired sample was delivered twice");
}
void RegistryRetirementRoutes()
{
    for (unsigned route = 0; route < 5; ++route)
    {
        Ring ring;
        DlssNr::VkRecording::Registry registry;
        registry.Allocate(1, 2, { 3 });
        registry.Begin(3);
        auto reservation = Reserve(ring, 1);
        auto ticket = registry.Current(1, 3);
        Check(ticket && registry.Pin(*ticket, { reservation.pin }), "retirement route pin failed");
        reservation.pin.reset();
        registry.End(3);
        Check(!ring.TakeRetired(0), "End retired route pin");
        switch (route)
        {
        case 0:
            registry.Reset(3);
            break;
        case 1:
            registry.ResetPool(1, 2);
            break;
        case 2:
            registry.Free(1, 2, 3);
            break;
        case 3:
            registry.DestroyPool(1, 2);
            break;
        case 4:
            registry.ForgetDevice(1);
            break;
        }
        auto sample = ring.TakeRetired(0);
        Check(sample && sample->serial == reservation.serial, "actual registry retirement did not publish sample");
    }
    Ring ring;
    DlssNr::VkRecording::Registry registry;
    registry.Allocate(1, 2, { 3 });
    registry.Begin(3);
    auto stale = registry.Current(1, 3);
    registry.End(3);
    auto reservation = Reserve(ring, 1);
    Check(stale && !registry.Pin(*stale, { reservation.pin }), "ended recording accepted pin");
    ring.Cancel(reservation); // No commands may have been recorded for this failed admission.
    auto retry = Reserve(ring, 1);
    Check(retry.serial > reservation.serial, "failed pin cancellation did not make slot reusable");
}
void MasksAndSaturation()
{
    Ring ring;
    Check(!ring.Reserve(0) && !ring.Reserve(1u << 4) && !ring.Reserve(1u << 31),
          "unready or out-of-range mask admitted slot");
    Reservation reservations[4];
    for (unsigned i = 0; i < 4; ++i)
    {
        reservations[i] = Reserve(ring, 1u << i);
        Check(reservations[i].slot == i && reservations[i].serial == i + 1, "wrong slot/serial");
        Check(!ring.TakeRetired(i), "live reservation was immediately retired");
    }
    allocationBudget = 0;
    auto saturated = ring.Reserve(15);
    allocationBudget = -1;
    Check(!saturated, "saturated ring admitted a fifth sample");
    reservations[2].pin.reset();
    Check(!ring.Reserve(15), "retired but unconsumed slot was overwritten");
    auto sample = ring.TakeRetired(2);
    Check(sample && sample->serial == 3, "wrong retired sample");
    auto replacement = Reserve(ring, 4);
    Check(replacement.slot == 2 && replacement.serial == 5, "failed reserve mutated serial or slot");
    Check(!ring.TakeRetired(4) && !ring.TakeRetired(UINT32_MAX), "invalid slot was readable");
}
void CancelIdentity()
{
    Ring ring, other;
    auto old = Reserve(ring, 1);
    auto foreign = Reserve(other, 1);
    ring.Cancel(foreign);
    Check(!ring.Reserve(1), "foreign reservation canceled occupied slot");
    auto wrongSerial = old;
    ++wrongSerial.serial;
    ring.Cancel(wrongSerial);
    Check(!ring.Reserve(1), "wrong serial canceled occupied slot");
    ring.Cancel(old);
    auto current = Reserve(ring, 1);
    ring.Cancel(old);
    Check(!ring.Reserve(1), "stale cancel cleared newer occupancy");
    old.pin.reset();
    Check(!ring.TakeRetired(0), "old token retired newer occupancy");
    current.pin.reset();
    auto sample = ring.TakeRetired(0);
    Check(sample && sample->serial == current.serial, "current token did not retire");
}
void InvalidationAndAbandonment()
{
    Ring ring;
    auto old = Reserve(ring, 1);
    ring.Invalidate();
    Check(!ring.Reserve(1) && !ring.TakeRetired(0), "invalidate freed live slot");
    old.pin.reset();
    auto sample = ring.TakeRetired(0);
    Check(sample && !sample->usable, "invalidated sample remained usable");
    auto abandoned = Reserve(ring, 1);
    ring.Abandon();
    auto current = Reserve(ring, 1);
    Check(current.serial > abandoned.serial, "abandon reused serial");
    ring.Cancel(abandoned);
    abandoned.pin.reset();
    Check(!ring.TakeRetired(0) && !ring.Reserve(1), "abandoned token affected newer occupancy");
    current.pin.reset();
    sample = ring.TakeRetired(0);
    Check(sample && sample->usable && sample->serial == current.serial, "new sample invalid after abandon");
    std::shared_ptr<void> surviving;
    {
        Ring temporary;
        surviving = Reserve(temporary).pin;
        temporary.Abandon();
    }
    surviving.reset(); // Must neither touch the destroyed ring nor perform GPU calls.
}
void OutOfOrder()
{
    Ring ring;
    auto older = Reserve(ring, 1), newer = Reserve(ring, 2);
    newer.pin.reset();
    auto second = ring.TakeRetired(1);
    Check(second && second->serial == newer.serial, "newer sample not independently retired");
    older.pin.reset();
    auto first = ring.TakeRetired(0);
    Check(first && first->serial == older.serial && first->serial < second->serial,
          "out-of-order retirement lost serial identity");
}
void CrossThreadPublication()
{
    Ring ring;
    auto reservation = Reserve(ring, 1);
    int published = 0;
    std::atomic<bool> start { false };
    std::thread worker(
        [pin = std::move(reservation.pin), &published, &start]() mutable
        {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            published = 97;
            pin.reset();
        });
    auto premature = ring.TakeRetired(0);
    start.store(true, std::memory_order_release);
    if (premature)
    {
        worker.join();
        Check(false, "token retired before producer released it");
    }
    std::optional<Sample> sample;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!(sample = ring.TakeRetired(0)) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    // Read before join: the token's release and TakeRetired's acquire publish this write.
    if (!sample)
    {
        worker.join();
        Check(false, "cross-thread token release did not retire");
    }
    const int observed = published;
    worker.join();
    Check(observed == 97 && sample->usable, "retirement did not publish preceding writes");
}
void AllocationFailure()
{
    bool sawFailure = false, sawSuccess = false;
    for (int budget = 0; budget < 8; ++budget)
    {
        Ring ring;
        auto existing = Reserve(ring, 1);
        allocationBudget = budget;
        std::optional<Reservation> attempt;
        try
        {
            attempt = ring.Reserve(2);
        }
        catch (...)
        {
            allocationBudget = -1;
            throw;
        }
        allocationBudget = -1;
        Check(!ring.TakeRetired(0), "OOM prematurely retired an existing reservation");
        if (!attempt)
        {
            sawFailure = true;
            attempt = ring.Reserve(2);
            Check(attempt && attempt->serial == 2, "allocation failure published state or consumed serial");
        }
        else
            sawSuccess = true;
        Check(attempt->slot == 1 && attempt->serial == 2, "unexpected post-allocation slot identity");
    }
    Check(sawFailure && sawSuccess, "allocation fault sweep did not exercise both outcomes");
}
void Overflow()
{
    Ring ring(std::numeric_limits<uint64_t>::max());
    auto last = Reserve(ring, 1);
    Check(last.serial == std::numeric_limits<uint64_t>::max(), "last representable serial missing");
    Check(!ring.Reserve(2), "serial overflow admitted reservation");
    ring.Cancel(last);
    ring.Abandon();
    Check(!ring.Reserve(15), "abandon reset serial exhaustion");
    Ring exhausted(0);
    Check(!exhausted.Reserve(15), "zero serial must mean exhausted");
}
static_assert(noexcept(std::declval<Ring&>().Cancel(std::declval<const Reservation&>())));
static_assert(noexcept(std::declval<Ring&>().TakeRetired(0)));
static_assert(noexcept(std::declval<Ring&>().Invalidate()));
static_assert(noexcept(std::declval<Ring&>().Abandon()));
int main()
{
    unsigned failed = 0;
    for (auto test : { RegistryLifetime, RegistryRetirementRoutes, MasksAndSaturation, CancelIdentity,
                       InvalidationAndAbandonment, OutOfOrder, CrossThreadPublication, AllocationFailure, Overflow })
        try
        {
            test();
        }
        catch (const std::exception& error)
        {
            ++failed;
            std::cerr << "FAIL: " << error.what() << '\n';
        }
    if (failed)
        return 1;
    std::cout
        << "PASS: meter readback recording lifetime, saturation, retirement, OOM and serial boundaries (host only)\n";
}
