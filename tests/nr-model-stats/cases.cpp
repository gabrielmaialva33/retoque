// tests/nr-model-stats: the model's own VRAM figure (docs/dlssnr/design/model-vram-stats.md).
//
// The forwarder's dlssnr_query_stats and the direct runtime's QueryStats are sliced out of their sources by run.py and
// compiled here, each in a namespace of its own, against a fake parameter block and fake stats callbacks: present,
// absent, refusing, silent and faulting. The block is typed as the driver's is: MSVC lays a class's overloaded virtuals
// out in reverse declaration order, so the pointer setter is slot 0 (getter 8) and the unsigned 64-bit setter slot 7
// (getter 15), and a value read through the other type's getter is not there. The reporting callback writes SizeInBytes
// through slot 7, as nvngx_dlssnr.dll's GetStatsCommon does (its call through vtable offset 0x38).
// The two run the same cases and must give the same answers. Guarded is a stand-in: the shipped one is SEH and is held
// verbatim to the forwarder by tests/nr-model-loader; this one turns a thrown FakeFault into the same sticky refusal.
//
// DlssNr_ModelStats.h, the host's log and menu text, is compiled as it is.

#include <dlssnr/DlssNr_ModelStats.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#define __cdecl
#define __thiscall

namespace
{
int g_failures = 0;

void Require(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// --- The fake parameter block --------------------------------------------------------------------------------------

constexpr int kNgxSuccess = 1;
constexpr int kNgxInvalidParameter = static_cast<int>(0xBAD00005u);

// One entry per name, typed by the setter that wrote it: setting a name through one type drops it from the other.
struct FakeBlock
{
    void** vt = nullptr; // first, as the driver's block keeps its vtable
    // Set(void*), slot 0 / Get(void**), slot 8.
    std::map<std::string, unsigned long long> ptr;
    // Set(unsigned long long), slot 7 / Get(unsigned long long*), slot 15.
    std::map<std::string, unsigned long long> u64;
    std::vector<std::string> calls; // "set NAME", "get NAME", "callback"
    bool getterFails = false;
};

void Set(FakeBlock* block, std::map<std::string, unsigned long long>& into,
         std::map<std::string, unsigned long long>& other, const char* name, unsigned long long value)
{
    block->calls.push_back(std::string("set ") + name);
    other.erase(name);
    into[name] = value;
}

int Get(FakeBlock* block, const std::map<std::string, unsigned long long>& from, const char* name,
        unsigned long long* out)
{
    block->calls.push_back(std::string("get ") + name);
    if (block->getterFails)
        return kNgxInvalidParameter;
    const auto found = from.find(name);
    if (found == from.end())
        return kNgxInvalidParameter;
    *out = found->second;
    return kNgxSuccess;
}

void FakeSetPtr(void* self, const char* name, unsigned long long value)
{
    auto* block = static_cast<FakeBlock*>(self);
    Set(block, block->ptr, block->u64, name, value);
}

int FakeGetPtr(void* self, const char* name, unsigned long long* out)
{
    auto* block = static_cast<FakeBlock*>(self);
    return Get(block, block->ptr, name, out);
}

void FakeSetU64(void* self, const char* name, unsigned long long value)
{
    auto* block = static_cast<FakeBlock*>(self);
    Set(block, block->u64, block->ptr, name, value);
}

int FakeGetU64(void* self, const char* name, unsigned long long* out)
{
    auto* block = static_cast<FakeBlock*>(self);
    return Get(block, block->u64, name, out);
}

// Slots the stats query has no business touching are null, so a call through one fails the run under ASan.
void* g_vtable[16] = {};

void InstallVtable()
{
    g_vtable[0] = reinterpret_cast<void*>(&FakeSetPtr);
    g_vtable[7] = reinterpret_cast<void*>(&FakeSetU64);
    g_vtable[8] = reinterpret_cast<void*>(&FakeGetPtr);
    g_vtable[15] = reinterpret_cast<void*>(&FakeGetU64);
}

// --- Fake stats callbacks -------------------------------------------------------------------------------------------

struct FakeFault
{
};

void* g_seenParams = nullptr;
int g_callbackCalls = 0;
// Above 4 GiB on purpose: a 32-bit read would lose it.
constexpr unsigned long long kReported = 5ull * 1024 * 1024 * 1024 + 812ull * 1024 * 1024 + 3;

int CbReports(void* params)
{
    ++g_callbackCalls;
    g_seenParams = params;
    auto* block = static_cast<FakeBlock*>(params);
    block->calls.push_back("callback");
    // Through the block's own slot 7, as the model's GetStatsCommon writes it; logged as the model's write.
    reinterpret_cast<void (*)(void*, const char*, unsigned long long)>(block->vt[7])(params, "SizeInBytes", kReported);
    block->calls.pop_back();
    return kNgxSuccess;
}

int CbRefuses(void* params)
{
    ++g_callbackCalls;
    static_cast<FakeBlock*>(params)->calls.push_back("callback");
    return kNgxInvalidParameter;
}

int CbSilent(void* params)
{
    ++g_callbackCalls;
    static_cast<FakeBlock*>(params)->calls.push_back("callback");
    return kNgxSuccess;
}

int CbFaults(void* params)
{
    ++g_callbackCalls;
    static_cast<FakeBlock*>(params)->calls.push_back("callback");
    throw FakeFault {};
}

unsigned long long Address(int (*callback)(void*))
{
    return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(callback)));
}

// --- What both loaders share in their sources -----------------------------------------------------------------------

constexpr int VT_SET_ULL = 0;
constexpr int VT_GET_ULL = VT_SET_ULL + 8;
constexpr int VT_SET_U64 = 7;
constexpr int VT_GET_U64 = VT_SET_U64 + 8;
constexpr int kFaulted = static_cast<int>(0xBAD0FA17u);
using PFN_SetULL = void(__thiscall*)(void*, const char*, unsigned long long);
using PFN_GetULL = int(__thiscall*)(void*, const char*, unsigned long long*);
using PFN_NrStatsCallback = int(__cdecl*)(void*);

// A stand-in for the shipped Guarded (see the top of this file): sticky, and nothing enters the model after a fault.
#define FAKE_GUARDED                                                                                                   \
    bool g_faulted = false;                                                                                            \
    template <typename Fn, typename... Args> int Guarded(Fn fn, Args... args)                                          \
    {                                                                                                                  \
        if (g_faulted)                                                                                                 \
            return kFaulted;                                                                                           \
        try                                                                                                            \
        {                                                                                                              \
            return fn(args...);                                                                                        \
        }                                                                                                              \
        catch (const FakeFault&)                                                                                       \
        {                                                                                                              \
            g_faulted = true;                                                                                          \
            return kFaulted;                                                                                           \
        }                                                                                                              \
    }

namespace Forwarder
{
struct
{
    bool initialised = false;
} g_snip;
int dlssnr_last_stats_result = 0;
FAKE_GUARDED
// @@FORWARDER@@
int Query(void* params, unsigned long long* out) { return dlssnr_query_stats(params, out); }
int LastResult() { return dlssnr_last_stats_result; }
void Reset(bool initialised)
{
    g_snip.initialised = initialised;
    g_faulted = false;
    dlssnr_last_stats_result = 12345;
}
} // namespace Forwarder

namespace Direct
{
struct
{
    bool initialised = false;
} g_snip;
int g_lastStatsResult = 0;
FAKE_GUARDED
// @@DIRECT@@
int Query(void* params, unsigned long long* out) { return QueryStats(params, out); }
int LastResult() { return *LastStatsResult(); }
void Reset(bool initialised)
{
    g_snip.initialised = initialised;
    g_faulted = false;
    g_lastStatsResult = 12345;
}
} // namespace Direct

// --- The cases, run against each loader -----------------------------------------------------------------------------

struct Loader
{
    const char* name;
    int (*query)(void*, unsigned long long*);
    int (*lastResult)();
    void (*reset)(bool);
};

FakeBlock NewBlock(int (*callback)(void*))
{
    FakeBlock block;
    block.vt = g_vtable;
    if (callback != nullptr)
        block.ptr["DLSSNRGetStatsCallback"] = Address(callback);
    return block;
}

// One outcome, recorded so the two loaders can be compared case by case.
struct Outcome
{
    int rc = 0;
    unsigned long long out = 0;
    int last = 0;
    std::vector<std::string> calls;
    int callbackCalls = 0;
    bool operator==(const Outcome&) const = default;
};

Outcome Run(const Loader& loader, FakeBlock& block, bool initialised = true)
{
    loader.reset(initialised);
    g_callbackCalls = 0;
    g_seenParams = nullptr;
    Outcome o;
    o.out = 0xABCDu;
    o.rc = loader.query(&block, &o.out);
    o.last = loader.lastResult();
    o.calls = block.calls;
    o.callbackCalls = g_callbackCalls;
    return o;
}

std::vector<Outcome> Cases(const Loader& loader)
{
    const std::string n = std::string(loader.name) + ": ";
    std::vector<Outcome> all;

    // Present: one sentinel write, the callback on the same block, the size read back whole as 64 bits.
    {
        auto block = NewBlock(&CbReports);
        const auto o = Run(loader, block);
        all.push_back(o);
        Require(o.rc == 1, n + "a published callback that answers reports");
        Require(o.out == kReported, n + "SizeInBytes is read as 64 bits, unchanged");
        Require(g_seenParams == &block, n + "the callback is handed the block it was read from");
        Require(o.last == kNgxSuccess, n + "the callback's result is kept for the debug line");
        const std::vector<std::string> expected = { "get DLSSNRGetStatsCallback", "set SizeInBytes", "callback",
                                                    "get SizeInBytes" };
        Require(o.calls == expected, n + "read the callback, clear the size, call, read the size -- in that order");
    }

    // Absent: nothing written, nothing called.
    {
        auto block = NewBlock(nullptr);
        const auto o = Run(loader, block);
        all.push_back(o);
        Require(o.rc == 0, n + "no callback in the block is 'not published'");
        Require(o.out == 0xABCDu, n + "an absent callback leaves the output alone");
        Require(o.callbackCalls == 0, n + "an absent callback is not called");
        Require(o.calls == std::vector<std::string> { "get DLSSNRGetStatsCallback" },
                n + "an absent callback writes nothing to the shared block");
        Require(o.last == 0, n + "no call, no result");
    }
    {
        auto block = NewBlock(nullptr);
        block.ptr["DLSSNRGetStatsCallback"] = 0;
        const auto o = Run(loader, block);
        all.push_back(o);
        Require(o.rc == 0 && o.callbackCalls == 0, n + "a null callback is 'not published'");
    }
    {
        auto block = NewBlock(&CbReports);
        block.getterFails = true;
        const auto o = Run(loader, block);
        all.push_back(o);
        Require(o.rc == 0 && o.callbackCalls == 0, n + "a block that will not answer is 'not published'");
    }

    // Before NGX is up in this loader the callback has no instance to count ("couldn't find initialized instance"):
    // not asked at all, and the block is not touched.
    {
        auto block = NewBlock(&CbReports);
        const auto o = Run(loader, block, false);
        all.push_back(o);
        Require(o.rc == 0 && o.callbackCalls == 0 && o.calls.empty(),
                n + "not asked before the model is initialised here");
    }

    // Null arguments.
    {
        loader.reset(true);
        unsigned long long out = 0;
        auto block = NewBlock(&CbReports);
        Require(loader.query(nullptr, &out) == 0, n + "a null block is refused");
        Require(loader.query(&block, nullptr) == 0 && block.calls.empty(), n + "a null output is refused untouched");
    }

    // Refusing: -1, the output untouched, the result kept.
    {
        auto block = NewBlock(&CbRefuses);
        const auto o = Run(loader, block);
        all.push_back(o);
        Require(o.rc == -1, n + "a refusing callback is -1");
        Require(o.out == 0xABCDu, n + "a refusal writes no size");
        Require(o.last == kNgxInvalidParameter, n + "the refusal's code is kept");
        Require(o.calls.size() == 3 && o.calls.back() == "callback", n + "a refusal is not followed by a size read");
    }

    // Silent: claims success, writes nothing. The sentinel is what tells it from a zero.
    {
        auto block = NewBlock(&CbSilent);
        block.u64["SizeInBytes"] = 777; // a stale figure from something else sharing the block
        const auto o = Run(loader, block);
        all.push_back(o);
        Require(o.rc == -1, n + "success without a size is -1, not a stale or zero size");
        Require(o.out == 0xABCDu, n + "a silent callback writes no size");
        Require(o.last == kNgxSuccess, n + "the silent callback's own answer is kept");
    }

    // Faulting: -1, sticky -- the next query, with a callback that would answer, does not enter the model.
    {
        auto block = NewBlock(&CbFaults);
        const auto o = Run(loader, block);
        all.push_back(o);
        Require(o.rc == -1, n + "a faulting callback is -1");
        Require(o.last == kFaulted, n + "a fault is recorded as kFaulted");
        Require(o.out == 0xABCDu, n + "a fault writes no size");
        Require(o.calls.size() == 3 && o.calls.back() == "callback", n + "no size is read after a fault");

        auto again = NewBlock(&CbReports);
        g_callbackCalls = 0;
        unsigned long long out = 0;
        const int rc = loader.query(&again, &out);
        Require(rc == -1 && g_callbackCalls == 0 && loader.lastResult() == kFaulted,
                n + "after a fault nothing enters the model again");
    }

    return all;
}

void BothLoadersAgree()
{
    InstallVtable();
    const Loader forwarder { "forwarder", &Forwarder::Query, &Forwarder::LastResult, &Forwarder::Reset };
    const Loader direct { "direct", &Direct::Query, &Direct::LastResult, &Direct::Reset };
    const auto a = Cases(forwarder);
    const auto b = Cases(direct);
    Require(a.size() == b.size(), "both loaders ran the same cases");
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        Require(a[i] == b[i], "the forwarder and the direct runtime answer case " + std::to_string(i) + " the same");
}

// --- DlssNr_ModelStats.h --------------------------------------------------------------------------------------------

namespace MS = DlssNr::ModelStats;

void MiBText()
{
    Require(MS::RoundedMiB(0) == 0, "0 bytes is 0 MiB");
    Require(MS::RoundedMiB(1024 * 1024) == 1, "1 MiB");
    Require(MS::RoundedMiB(1024 * 1024 + 512 * 1024 - 1) == 1, "rounds down below the half");
    Require(MS::RoundedMiB(1024 * 1024 + 512 * 1024) == 2, "rounds the half up");
    Require(MS::RoundedMiB(kReported) == 5 * 1024 + 812, "above 4 GiB, whole");
    Require(MS::RoundedMiB(~0ull - 1) == (~0ull - 1) / (1024 * 1024) + 1, "the top of the range does not wrap");
    Require(MS::MiBText(812ull * 1024 * 1024) == "812 MiB", "MiBText: whole MiB");
    Require(MS::MiBText(1000) == "under 1 MiB", "MiBText: a figure below half a MiB is not called zero");
    Require(MS::MiBText(0) == "0 MiB", "MiBText: zero is zero");
}

void Lines()
{
    const unsigned long long bytes = 812ull * 1024 * 1024;
    Require(MS::InfoLine(bytes, true) == "DLSS-NR: the model's feature uses 812 MiB (851443712 bytes, its own count)",
            "the first build's line: " + MS::InfoLine(bytes, true));
    const auto later = MS::InfoLine(bytes, false);
    Require(later.rfind("DLSS-NR: the model's features use 812 MiB (851443712 bytes, its own count", 0) == 0 &&
                later.find("retired") != std::string::npos,
            "a later build says the count may include a feature being retired: " + later);

    Require(MS::Unreported(MS::kNoQuery, 0).find("predates") != std::string::npos,
            "an old forwarder without the export is named: " + MS::Unreported(MS::kNoQuery, 0));
    Require(MS::Unreported(0, 0).find("publishes no stats callback") != std::string::npos,
            "an absent callback is named: " + MS::Unreported(0, 0));
    Require(MS::Unreported(-1, 1).find("without a size") != std::string::npos,
            "success without a size is named: " + MS::Unreported(-1, 1));
    Require(MS::Unreported(-1, kNgxInvalidParameter).find("0xBAD00005") != std::string::npos,
            "a refusal's code is printed as an unsigned hex: " + MS::Unreported(-1, kNgxInvalidParameter));
    Require(MS::Unreported(-1, kFaulted).find("faulted") != std::string::npos,
            "a fault is named: " + MS::Unreported(-1, kFaulted));
    for (const auto& line : { MS::Unreported(MS::kNoQuery, 0), MS::Unreported(0, 0), MS::Unreported(-1, 1),
                              MS::Unreported(-1, kNgxInvalidParameter), MS::Unreported(-1, kFaulted) })
        Require(line.rfind("DLSS-NR: no VRAM figure from the model (", 0) == 0 && line.back() == ')',
                "every debug line has one shape: " + line);

    Require(!MS::Known(MS::kUnknown), "the unknown value is not a figure");
    Require(MS::Known(0) && MS::Known(kReported), "zero and large figures are figures");
}
} // namespace

int main()
{
    BothLoadersAgree();
    MiBText();
    Lines();

    if (g_failures != 0)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS: model stats cases\n");
    return 0;
}
