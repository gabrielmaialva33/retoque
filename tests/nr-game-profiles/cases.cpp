// DlssNr_GameProfiles.h as shipped: the table, the exe match, the apply rules against a fake option with the
// CustomOptional surface the production code uses (has_value, set_volatile_value), and the WINEDLLPATH search.

#include "dlssnr/DlssNr_GameProfiles.h"

#include <cstdio>
#include <set>
#include <string>

namespace
{
int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char* what, int line)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

template <typename T> struct FakeOption
{
    std::optional<T> value;
    bool isVolatile = false;
    bool has_value() const { return value.has_value(); }
    void set_volatile_value(const T& v)
    {
        value = v;
        isVolatile = true;
    }
};

enum class FakeUpscaler
{
    Fsr22,
    Dlss,
    Dlss12,
    Unknown
};

FakeUpscaler ToUpscaler(const char* code)
{
    const std::string c = code;
    return c == "dlss" ? FakeUpscaler::Dlss : c == "dlss_12" ? FakeUpscaler::Dlss12 : FakeUpscaler::Unknown;
}

using namespace DlssNr::GameProfiles;

void Matching()
{
    const Profile* divinity = Match("EoCApp.exe");
    CHECK(divinity != nullptr && std::string(divinity->name) == "Divinity: Original Sin 2");
    CHECK(Match("eocapp.exe") == divinity);
    CHECK(Match("EOCAPP.EXE") == divinity);
    CHECK(Match("EoCApp") == nullptr);          // no extension: not the file name
    CHECK(Match("EoCApp.exe.bak") == nullptr);  // near miss
    CHECK(Match("SupportTool.exe") == nullptr); // Divinity's other exe
    CHECK(Match("witcher3.exe") == nullptr);    // the DX12 renderer must not get HookMethod=2
    CHECK(Match("CrimsonDesert.exe") == nullptr);
    CHECK(Match("") == nullptr);

    const Profile* doom = Match("DOOMEternalx64vk.exe");
    CHECK(doom != nullptr && doom->vulkanPresentHost == true && !doom->hookMethod);
    const Profile* sotf = Match("SonsOfTheForest.exe");
    CHECK(sotf != nullptr && sotf->hookMethod == 2u && sotf->dx11Upscaler != nullptr &&
          std::string(sotf->dx11Upscaler) == "dlss");
    const Profile* hellraiser = Match("Hellraiser-Win64-Shipping.exe");
    CHECK(hellraiser != nullptr && hellraiser->hookMethod == 2u && hellraiser->zeroGuideReset == true);
    CHECK(Match("GenerationZero_F.exe") != nullptr);
    CHECK(Match("pcsx2-qt.exe") != nullptr);
}

void TableIntegrity()
{
    std::set<std::string> seen;
    for (const auto& p : Table())
    {
        CHECK(p.name != nullptr && p.name[0] != 0);
        CHECK(!p.exes.empty());
        CHECK(p.evidence != nullptr && p.evidence[0] != 0);
        // Something to apply, and nothing that turns NR on or picks a look: only route keys exist in Profile.
        CHECK(p.hookMethod || p.zeroGuideReset || p.vulkanPresentHost || p.dx11Upscaler != nullptr);
        if (p.hookMethod)
            CHECK(*p.hookMethod <= 2u);
        if (p.dx11Upscaler != nullptr)
            CHECK(ToUpscaler(p.dx11Upscaler) != FakeUpscaler::Unknown);
        for (const auto exe : p.exes)
        {
            CHECK(Lower(exe) == std::string(exe)); // stored lowercase, or Match would never find it
            CHECK(exe.size() > 4 && exe.substr(exe.size() - 4) == ".exe");
            CHECK(seen.insert(std::string(exe)).second); // one profile per exe
        }
    }
}

void ApplyRules()
{
    const Profile& sotf = *Match("sonsoftheforest.exe");

    // Nothing set by the user: every key of the profile goes in, volatile.
    {
        FakeOption<unsigned> hook;
        FakeOption<bool> zero, vk;
        FakeOption<FakeUpscaler> dx11;
        const auto r = ApplyProfile(sotf, hook, zero, vk, dx11, ToUpscaler);
        CHECK(hook.value == 2u && hook.isVolatile);
        CHECK(dx11.value == FakeUpscaler::Dlss && dx11.isVolatile);
        CHECK(!zero.value && !vk.value); // keys the profile does not name stay untouched
        CHECK(r.size() == 2 && r[0].key == "HookMethod" && r[0].applied && r[1].key == "Dx11Upscaler" && r[1].applied);
    }
    // The user's ini wins, key by key.
    {
        FakeOption<unsigned> hook;
        hook.value = 1u;
        FakeOption<bool> zero, vk;
        FakeOption<FakeUpscaler> dx11;
        dx11.value = FakeUpscaler::Dlss12;
        const auto r = ApplyProfile(sotf, hook, zero, vk, dx11, ToUpscaler);
        CHECK(hook.value == 1u && !hook.isVolatile);
        CHECK(dx11.value == FakeUpscaler::Dlss12 && !dx11.isVolatile);
        CHECK(r.size() == 2 && !r[0].applied && !r[1].applied);
    }
    // Mixed: one set by the user, one not.
    {
        const Profile& hell = *Match("hellraiser-win64-shipping.exe");
        FakeOption<unsigned> hook;
        FakeOption<bool> zero, vk;
        zero.value = false;
        FakeOption<FakeUpscaler> dx11;
        const auto r = ApplyProfile(hell, hook, zero, vk, dx11, ToUpscaler);
        CHECK(hook.value == 2u && hook.isVolatile);
        CHECK(zero.value == false && !zero.isVolatile);
        CHECK(r.size() == 2 && r[0].applied && !r[1].applied && r[1].value == "true");
    }
}

void Nvapi()
{
    const std::string want = "Z:\\opt\\proton\\files\\lib\\wine\\nvapi\\x86_64-windows\\nvapi64.dll";
    auto onlyWant = [&](const std::string& p) { return p == want; };
    auto none = [](const std::string&) { return false; };
    auto all = [](const std::string&) { return true; };

    CHECK(FindProtonNvapi("/opt/proton/files/lib/vkd3d:/opt/proton/files/lib/wine", onlyWant) == want);
    CHECK(FindProtonNvapi("/opt/proton/files/lib/vkd3d:/opt/proton/files/lib/wine/", onlyWant) == want);
    // The file must exist: a Proton without it gives nothing.
    CHECK(!FindProtonNvapi("/opt/proton/files/lib/vkd3d:/opt/proton/files/lib/wine", none));
    // Only entries that end in /wine are searched; vkd3d's is never taken even if something answers there.
    CHECK(FindProtonNvapi("/opt/proton/files/lib/vkd3d", all) == std::nullopt);
    CHECK(FindProtonNvapi("/a/winex:/b/wine", all) == "Z:\\b\\wine\\nvapi\\x86_64-windows\\nvapi64.dll");
    // Relative or empty entries are ignored.
    CHECK(FindProtonNvapi("relative/wine::", all) == std::nullopt);
    CHECK(FindProtonNvapi("", all) == std::nullopt);
    // The first match wins, in WINEDLLPATH order.
    CHECK(FindProtonNvapi("/x/wine:/y/wine", all) == "Z:\\x\\wine\\nvapi\\x86_64-windows\\nvapi64.dll");
    // The nvml entry Proton prepends for some games ends in /wine too, and has no nvapi: skipped by the file test.
    CHECK(FindProtonNvapi("/p/files/lib/wine/nvidia-libs/nvml/wine:/p/files/lib/vkd3d:/p/files/lib/wine",
                          [](const std::string& p) { return p.find("nvml") == std::string::npos; }) ==
          "Z:\\p\\files\\lib\\wine\\nvapi\\x86_64-windows\\nvapi64.dll");
}
} // namespace

int main()
{
    Matching();
    TableIntegrity();
    ApplyRules();
    Nvapi();

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: %d checks (exe match, table integrity, apply rules, Proton nvapi64 search)\n", g_checks);
    return 0;
}
