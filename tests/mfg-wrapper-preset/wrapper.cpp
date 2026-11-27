// Production wrapper orchestration slice with bounded fake modules and Win32 adapters.
#include "MfgWrapperPolicy.h"
#include <array>
#include <cassert>
#include <format>
#include <string>
#include <vector>
#include <cstdio>
using HMODULE = void*;
struct version_t
{
    unsigned major = 2, minor = 7;
    version_t(unsigned a = 2, unsigned b = 7, unsigned = 0, unsigned = 0) : major(a), minor(b) {}
    bool operator>=(const version_t& other) const
    {
        return major > other.major || (major == other.major && minor >= other.minor);
    }
};
struct FakeModule
{
    std::vector<uint8_t> bytes { 0xBA, 3, 0, 0, 0, 0x3B, 0xCA, 0x0F, 0x42, 0xD1 };
    version_t version;
};
struct State
{
    HMODULE optiSlDLSSG = nullptr;
    static State& Instance()
    {
        static State s;
        return s;
    }
};
HMODULE game = nullptr, interposer = nullptr;
bool legacy = false;
HMODULE GetModuleHandleW(const wchar_t* name) { return std::wstring(name) == L"sl.interposer.dll" ? interposer : game; }
void* GetProcAddress(HMODULE, const char* name)
{
    return legacy && std::string(name) == "slGetFeatureFunction" ? nullptr : (void*) 1;
}
version_t ModuleVersion(HMODULE m) { return static_cast<FakeModule*>(m)->version; }
std::string Describe(HMODULE) { return "fake"; }
std::vector<MfgWrapper::Section> WrapperSections(HMODULE m)
{
    auto& b = static_cast<FakeModule*>(m)->bytes;
    return { { b.data(), b.size(), true } };
}
unsigned writes = 0, warnings = 0;
struct LiveOps
{
    bool Read(uintptr_t at, void* dest, size_t n)
    {
        std::memcpy(dest, (void*) at, n);
        return true;
    }
    bool Write(uintptr_t at, const void* src, size_t n)
    {
        ++writes;
        std::memcpy((void*) at, src, n);
        return true;
    }
};
#define LOG_INFO(...)                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
    } while (false)
#define LOG_WARN(...)                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        ++warnings;                                                                                                    \
    } while (false)
#define LOG_ERROR(...)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        ++warnings;                                                                                                    \
    } while (false)
constexpr uint8_t kMaxGeneratedFrames = 5;
// @@PLAN@@
enum class PatchStatus
{
    Ok,
    Already,
    Skip,
    Miss,
    Broken
};
PatchStatus FromOutcome(MfgPatch::Outcome o)
{
    return o == MfgPatch::Outcome::Committed        ? PatchStatus::Ok
           : o == MfgPatch::Outcome::AlreadyApplied ? PatchStatus::Already
                                                    : PatchStatus::Broken;
}
bool Applied(PatchStatus s) { return s == PatchStatus::Ok || s == PatchStatus::Already; }
bool Faulted(PatchStatus s) { return !Applied(s) && s != PatchStatus::Skip; }
const char* Status(PatchStatus) { return "test"; }
unsigned drsAttempts = 0;
bool PlanSlDrsClamp(HMODULE, std::vector<MfgPatch::Site>&, LiveOps&)
{
    ++drsAttempts;
    return false;
}
void Apply(HMODULE wrapper = nullptr)
{
    static MfgWrapper::Attempts wrappersDone;
    LiveOps ops;
    bool updated = false;
    // @@APPLY@@
    (void) updated;
}
int main()
{
    FakeModule first, second, third, bad, old;
    game = &first;
    Apply(&first);
    Apply(&second); // explicit private load while name lookup still returns the game
    assert(first.bytes[1] == 5 && second.bytes[1] == 5 && writes == 2);
    Apply(&first);
    assert(writes == 2);
    first.bytes[1] = 3;
    Apply(&first);
    assert(writes == 3 && first.bytes[1] == 5);
    Apply(&third);
    assert(writes == 4 && third.bytes[1] == 5);
    bad.bytes[1] = 4;
    const auto before = drsAttempts;
    Apply(&bad);
    assert(writes == 4 && bad.bytes[1] == 4 && drsAttempts == before);
    const auto warned = warnings;
    Apply(&bad);
    assert(warnings == warned && writes == 4);
    old.version.major = 1;
    Apply(&old);
    assert(old.bytes[1] == 3 && writes == 4);
    FakeModule legacyWrapper;
    interposer = (void*) 1;
    legacy = true;
    Apply(&legacyWrapper);
    assert(legacyWrapper.bytes[1] == 3 && writes == 4);
    interposer = nullptr;
    FakeModule ambiguous;
    const auto copy = ambiguous.bytes;
    ambiguous.bytes.insert(ambiguous.bytes.end(), copy.begin(), copy.end());
    Apply(&ambiguous);
    assert(ambiguous.bytes[1] == 3 && writes == 4);
    std::puts("Production wrapper orchestration: PASS");
}
