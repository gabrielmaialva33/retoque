#include "DlssNr_VkMeterReadback.h"
#include "DlssNr_VkRecording.h"
#include "DlssNr_Exposure.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

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
#define LOG_WARN(...) ((void) 0)
#define LOG_INFO(...) ((void) 0)
#define LOG_DEBUG(...) ((void) 0)
using VkBuffer = uint64_t;
using VkDeviceMemory = uint64_t;
constexpr uint64_t VK_NULL_HANDLE = 0;
constexpr unsigned long long kMeterSlots = 4;
namespace DlssNr
{
struct State
{
    VkBuffer meterReadback[4] = {};
    VkDeviceMemory meterReadbackMemory[4] = {};
    void* meterMapped[4] = {};
    unsigned long long meterFrames = 0;
    VkMeterReadback::Ring meterPending;
    uint64_t meterAcceptedSerial = 0;
    float gameExposure = 0.0f;
    float gamePreExposure = 1.0f;
    Exposure::PlausibilityLatch exposureLatch;
} g_vk;
std::array<std::array<float, 64>, 4> backing {};
void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
void ResetFixture()
{
    g_vk.meterPending.Abandon();
    VkRecording::Recordings().ForgetDevice(1);
    g_vk.meterFrames = 0;
    g_vk.meterAcceptedSerial = 0;
    g_vk.gameExposure = 0.125f;
    g_vk.gamePreExposure = 1.0f;
    g_vk.exposureLatch.Reset();
    for (unsigned i = 0; i < 4; ++i)
    {
        backing[i].fill(37.0f);
        g_vk.meterReadback[i] = 100 + i;
        g_vk.meterReadbackMemory[i] = 200 + i;
        g_vk.meterMapped[i] = backing[i].data();
    }
}
VkRecording::Ticket Begin(uint64_t buffer)
{
    auto& registry = VkRecording::Recordings();
    registry.Allocate(1, 2, { buffer });
    registry.Begin(buffer);
    auto ticket = registry.Current(1, buffer);
    Check(ticket.has_value(), "registry did not begin fixture recording");
    return *ticket;
}

// PRODUCTION_FUNCTIONS

void HeldRecordingIsUnreadable()
{
    ResetFixture();
    auto ticket = Begin(10);
#if TEST_FEATURE_HELPERS
    auto reservation = ReserveMeterReadback(ticket);
#else
    auto reservation = g_vk.meterPending.Reserve(1);
    Check(reservation && VkRecording::Recordings().Pin(ticket, { reservation->pin }), "baseline fixture pin failed");
#endif
    Check(reservation.has_value(), "readback reservation failed");
    const auto slot = reservation->slot;
    const auto serial = reservation->serial;
    reservation->pin.reset();
    backing[slot][0] = 0.5f; // Model a positive mapped value; no GPU work is executed.
    g_vk.meterFrames = 4;
    ConsumeMeterReadback();
    Check(g_vk.gameExposure == 0.125f && g_vk.meterAcceptedSerial == 0,
          "live recording pin did not prevent mapped exposure publication at frame four");
    VkRecording::Recordings().End(10);
    for (unsigned frame = 0; frame < 12; ++frame)
    {
        g_vk.meterFrames += 4;
        ConsumeMeterReadback();
        Check(g_vk.gameExposure == 0.125f, "executable/resubmittable recording became readable with age");
    }
    VkRecording::Recordings().Reset(10);
    ConsumeMeterReadback();
    Check(g_vk.gameExposure == 0.5f && g_vk.meterAcceptedSerial == serial,
          "retired recording did not publish valid mapped exposure");
    ConsumeMeterReadback();
    Check(g_vk.meterAcceptedSerial == serial, "consumption was not idempotent");
}
#if TEST_FEATURE_HELPERS
std::pair<uint32_t, uint64_t> Record(uint64_t buffer, float value)
{
    auto reservation = ReserveMeterReadback(Begin(buffer));
    Check(reservation.has_value(), "feature helper reservation failed");
    Check(backing[reservation->slot][0] == 0.0f, "successful admission did not clear float-zero sentinel");
    Check(backing[reservation->slot][1] == 37.0f, "sentinel clear modified another texel");
    backing[reservation->slot][0] = value;
    return { reservation->slot, reservation->serial };
}
void InvalidAndZeroSamples()
{
    for (float value :
         { 0.0f, -1.0f, 1e-7f, 1e7f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
    {
        ResetFixture();
        Record(20, value);
        VkRecording::Recordings().Reset(20);
        ConsumeMeterReadback();
        Check(g_vk.gameExposure == 0.125f && g_vk.meterAcceptedSerial == 0,
              "invalid/sentinel value replaced held exposure or consumed accepted serial");
    }
    ResetFixture();
    auto reservation = ReserveMeterReadback(Begin(21));
    Check(reservation.has_value(), "unexecuted fixture reserve failed");
    reservation->pin.reset();
    VkRecording::Recordings().Reset(21); // Recorded then retired without a modeled copy.
    ConsumeMeterReadback();
    Check(g_vk.gameExposure == 0.125f, "unexecuted zero sentinel replaced held exposure");
}
void ReadinessAndSaturation()
{
    ResetFixture();
    g_vk.meterReadbackMemory[0] = 0;
    g_vk.meterMapped[1] = nullptr;
    g_vk.meterReadback[3] = 0;
    auto selected = ReserveMeterReadback(Begin(30));
    Check(selected && selected->slot == 2, "feature admitted incomplete allocation tuple");
    Check(backing[0][0] == 37.0f && backing[1][0] == 37.0f && backing[3][0] == 37.0f,
          "unready mapped storage was modified");
    ResetFixture();
    for (uint64_t i = 0; i < 4; ++i)
        Record(31 + i, static_cast<float>(i + 1));
    const auto before = backing;
    auto rejected = ReserveMeterReadback(Begin(35));
    Check(!rejected && backing == before, "saturated admission overwrote a pending sample/sentinel");
}
void FailedPinAndAllocation()
{
    ResetFixture();
    auto ticket = Begin(40);
    VkRecording::Recordings().End(40);
    Check(!ReserveMeterReadback(ticket), "stale recording admitted readback");
    Check(backing[0][0] == 37.0f, "failed pin cleared sentinel before admission");
    auto retry = ReserveMeterReadback(Begin(40));
    Check(retry && retry->slot == 0, "failed pin left slot occupied");
    for (int budget = 0; budget < 9; ++budget)
    {
        ResetFixture();
        ticket = Begin(41);
        allocationBudget = budget;
        std::optional<VkMeterReadback::Reservation> attempt;
        try
        {
            attempt = ReserveMeterReadback(ticket);
        }
        catch (...)
        {
            allocationBudget = -1;
            throw;
        }
        allocationBudget = -1;
        if (!attempt)
        {
            Check(backing[0][0] == 37.0f, "allocation/pin failure touched mapped storage");
            attempt = ReserveMeterReadback(ticket);
            Check(attempt && attempt->slot == 0, "allocation/pin failure did not cancel occupancy");
        }
    }
}
void NewestValidAndOldRetirement()
{
    ResetFixture();
    auto old = Record(50, 0.2f);
    auto middle = Record(51, 0.4f);
    VkRecording::Recordings().Reset(50);
    ConsumeMeterReadback();
    Check(g_vk.meterAcceptedSerial == old.second, "initial sample was not consumed");
    auto newest = Record(52, 0.7f);
    Check(newest.first < middle.first, "fixture did not reverse slot/serial scan order");
    VkRecording::Recordings().Reset(51);
    VkRecording::Recordings().Reset(52);
    ConsumeMeterReadback();
    Check(g_vk.gameExposure == 0.7f && g_vk.meterAcceptedSerial == newest.second,
          "consumer did not choose newest valid serial across slot order");
    // A separate still-live older sample must not overwrite an accepted newer one.
    ResetFixture();
    old = Record(53, 0.3f);
    newest = Record(54, 0.8f);
    VkRecording::Recordings().Reset(54);
    ConsumeMeterReadback();
    VkRecording::Recordings().Reset(53);
    ConsumeMeterReadback();
    Check(g_vk.gameExposure == 0.8f && g_vk.meterAcceptedSerial == newest.second,
          "late older retirement overwrote newer accepted exposure");
}
void InvalidationAndForget()
{
    ResetFixture();
    Record(60, 0.6f);
    g_vk.meterPending.Invalidate();
    VkRecording::Recordings().Reset(60);
    ConsumeMeterReadback();
    Check(g_vk.gameExposure == 0.125f && g_vk.meterAcceptedSerial == 0, "invalidated pending sample was accepted");
    Record(61, 0.9f);
    g_vk.meterFrames = 77;
    ForgetMeterReadback();
    for (unsigned i = 0; i < 4; ++i)
    {
        Check(g_vk.meterReadback[i] == 0 && g_vk.meterReadbackMemory[i] == 0 && !g_vk.meterMapped[i],
              "forget did not clear all GPU handle/map metadata");
        g_vk.meterMapped[i] = reinterpret_cast<void*>(uintptr_t { 1 });
    }
    Check(g_vk.meterFrames == 0, "forget retained meter frame count");
    VkRecording::Recordings().ForgetDevice(1); // Releases token after its device metadata was forgotten.
    ConsumeMeterReadback();                    // Any stale age-based mapped read would fault on address 1.
    Check(g_vk.gameExposure == 0.125f, "forgotten sample was published");
}
#endif
} // namespace DlssNr
int main()
{
    unsigned failed = 0;
    using namespace DlssNr;
    for (auto test : { HeldRecordingIsUnreadable
#if TEST_FEATURE_HELPERS
                       ,
                       InvalidAndZeroSamples, ReadinessAndSaturation, FailedPinAndAllocation,
                       NewestValidAndOldRetirement, InvalidationAndForget
#endif
         })
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
    std::cout << "PASS: actual Vulkan feature readback helper lifetime and exposure consumption (host only)\n";
}
