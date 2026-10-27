#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <list>
#include <memory>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <vector>

std::atomic<int> failAfter { -1 };
#if defined(_MSC_VER)
#define NR_NOINLINE __declspec(noinline)
#else
#define NR_NOINLINE __attribute__((noinline))
#endif
void* operator new(std::size_t size)
{
    const int remaining = failAfter.load();
    if (remaining == 0)
        throw std::bad_alloc();
    if (remaining > 0)
        failAfter.fetch_sub(1);
    if (void* allocation = std::malloc(size == 0 ? 1 : size))
        return allocation;
    throw std::bad_alloc();
}
NR_NOINLINE void operator delete(void* allocation) noexcept { std::free(allocation); }
NR_NOINLINE void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }
#undef NR_NOINLINE

#ifdef NR_TEST_ALLOCATING_LIST
// A permitted STL implementation strategy: constructing even an empty list allocates its sentinel. The runner
// substitutes only the registry's list type; all ownership/lifecycle code remains the production header.
template <class T> struct AllocatingDefaultList : std::list<T>
{
    std::unique_ptr<char> sentinel = std::make_unique<char>();
};
template <class Key, class Value> struct AllocatingDefaultMap : std::unordered_map<Key, Value>
{
    std::unique_ptr<char> sentinel = std::make_unique<char>();
};
#endif

#include <dlssnr/DlssNr_VkRecording.h>

using namespace DlssNr::VkRecording;

void Check(bool condition, const char* text)
{
    if (!condition)
        throw std::runtime_error(text);
}

struct FailAllocations
{
    explicit FailAllocations(int count = 0) { failAfter.store(count); }
    ~FailAllocations() { failAfter.store(-1); }
};

std::weak_ptr<int> Pin(Registry& registry, Handle buffer, int& released)
{
    registry.Begin(buffer);
    const auto ticket = registry.Current(1, buffer);
    Check(ticket.has_value(), "setup must have an open recording");
    auto token = std::shared_ptr<int>(nullptr,
                                      [&registry, &released](int*)
                                      {
                                          // Reentry also proves that the record's final owner is released outside the
                                          // registry mutex.
                                          (void) registry.Current(1, 11);
                                          ++released;
                                      });
    Check(registry.Pin(*ticket, { token }), "setup must pin the resource");
    return token;
}

void Retire(const char* operation)
{
    Registry registry;
    registry.Allocate(1, 5, { 11, 12 });
    registry.Allocate(1, 6, { 13 });
    int released = 0;
    auto first = Pin(registry, 11, released);
    auto second = Pin(registry, 12, released);
    auto other = Pin(registry, 13, released);
    const std::vector<Handle> freed { 11, 12 };
    const std::vector<Handle> reallocated { 11 };
    bool threw = false;
    {
        FailAllocations fail;
        try
        {
            if (std::strcmp(operation, "begin") == 0)
                registry.Begin(11);
            else if (std::strcmp(operation, "reset") == 0)
                registry.Reset(11);
            else if (std::strcmp(operation, "reset-pool") == 0)
                registry.ResetPool(1, 5);
            else if (std::strcmp(operation, "destroy-pool") == 0)
                registry.DestroyPool(1, 5);
            else if (std::strcmp(operation, "forget-device") == 0)
                registry.ForgetDevice(1);
            else if (std::strcmp(operation, "free-vector") == 0)
                registry.Free(1, 5, freed);
            else if (std::strcmp(operation, "replace-allocation") == 0)
                registry.Allocate(1, 7, reallocated);
        }
        catch (const std::bad_alloc&)
        {
            threw = true;
        }
    }
    Check(!threw, "recording retirement must not allocate or throw bad_alloc");
    Check(first.expired(), "retirement must release the selected recording despite OOM");
    const bool one = std::strcmp(operation, "begin") == 0 || std::strcmp(operation, "reset") == 0 ||
                     std::strcmp(operation, "replace-allocation") == 0;
    Check(second.expired() != one, "retirement must affect exactly the selected buffer or pool");
    Check(other.expired() == (std::strcmp(operation, "forget-device") == 0),
          "pool/buffer retirement must preserve the other pool");
    Check(released == (one ? 1 : (other.expired() ? 3 : 2)), "each retired owner must be released once");
}

void DestructorDoesNotAllocate()
{
    auto registry = std::make_unique<Registry>();
    registry->Allocate(1, 5, { 11, 12 });
    int released = 0;
    auto first = Pin(*registry, 11, released);
    auto second = Pin(*registry, 12, released);
    {
        FailAllocations fail;
        registry.reset();
    }
    Check(first.expired() && second.expired() && released == 2,
          "registry destruction under OOM must retire every owner outside its mutex");
}

void ColdRegistryDoesNotAllocate()
{
    const std::vector<Handle> buffers { 11 };
    const std::vector<std::shared_ptr<void>> emptyPins;
    bool threw = false;
    bool admitted = false;
    {
        FailAllocations fail;
        try
        {
            auto& registry = Recordings();
            registry.Begin(11);
            registry.End(11);
            registry.Reset(11);
            registry.ResetPool(1, 5);
            registry.Free(1, 5, buffers);
            registry.DestroyPool(1, 5);
            registry.ForgetDevice(1);
            admitted = registry.Current(1, 11).has_value() || registry.Pin(Ticket { 1, 11, 1 }, emptyPins);
        }
        catch (const std::bad_alloc&)
        {
            threw = true;
        }
    }
    Check(!threw && !admitted, "first callback must construct the registry without allocation and reject unknown work");
}

void PinFailureIsContainedAndAtomic()
{
    bool eventuallySucceeded = false;
    for (int failure = 0; failure < 10; ++failure)
    {
        Registry registry;
        registry.Allocate(1, 5, { 11 });
        registry.Begin(11);
        const auto ticket = *registry.Current(1, 11);
        auto existing = std::make_shared<int>(1);
        auto added = std::make_shared<int>(2);
        Check(registry.Pin(ticket, { existing }), "setup must pin the existing owner");
        const std::vector<std::shared_ptr<void>> requested { existing, added, added };
        const long existingCount = existing.use_count();
        const long addedCount = added.use_count();
        bool threw = false;
        bool accepted = false;
        {
            FailAllocations fail(failure);
            try
            {
                accepted = registry.Pin(ticket, requested);
            }
            catch (const std::bad_alloc&)
            {
                threw = true;
            }
        }
        Check(!threw, "Pin must contain every allocation failure, including empty-list construction");
        Check(existing.use_count() == existingCount, "Pin must not retain duplicate ownership");
        Check(added.use_count() == addedCount + (accepted ? 1 : 0),
              "Pin must retain the whole unique batch or none after allocation failure");
        if (accepted)
        {
            eventuallySucceeded = true;
            break;
        }
    }
    Check(eventuallySucceeded, "Pin must recover when the failure point is beyond all required allocations");
}

void ManyRecordsRetireWithoutAllocation()
{
    Registry registry;
    int released = 0;
    constexpr Handle count = 4096;
    for (Handle buffer = 1; buffer <= count; ++buffer)
    {
        registry.Allocate(1, 5, { buffer });
        Pin(registry, buffer, released);
    }
    bool threw = false;
    {
        FailAllocations fail;
        try
        {
            registry.ForgetDevice(1);
        }
        catch (const std::bad_alloc&)
        {
            threw = true;
        }
    }
    Check(!threw && released == static_cast<int>(count),
          "device retirement must drain a large set of owner batches without allocation or lost callbacks");
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    try
    {
        if (std::strcmp(argv[1], "cold-registry") == 0)
            ColdRegistryDoesNotAllocate();
        else if (std::strcmp(argv[1], "destructor") == 0)
            DestructorDoesNotAllocate();
        else if (std::strcmp(argv[1], "pin-failure") == 0)
            PinFailureIsContainedAndAtomic();
        else if (std::strcmp(argv[1], "many-records") == 0)
            ManyRecordsRetireWithoutAllocation();
        else
            Retire(argv[1]);
        std::cout << "PASS: " << argv[1] << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        failAfter.store(-1);
        std::cerr << "FAIL: " << argv[1] << ": " << error.what() << '\n';
        return 1;
    }
}
