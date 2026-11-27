#include "MfgWrapperPolicy.h"
#include "DlssgPresetPolicy.h"
#include <cassert>
#include <cstdio>
#include <vector>

struct Memory
{
    bool Read(uintptr_t at, void* to, size_t n)
    {
        std::memcpy(to, (void*) at, n);
        return true;
    }
    bool Write(uintptr_t at, const void* from, size_t n)
    {
        std::memcpy((void*) at, from, n);
        return true;
    }
};
struct Setting
{
    uint32_t version, settingId, settingType, settingLocation;
    uint32_t isCurrentPredefined, isPredefinedValid;
    uint16_t settingName[64];
    uint32_t u32CurrentValue, u32PredefinedValue;
};

int main()
{
    using namespace MfgWrapper;
    std::vector<uint8_t> edx { 0xBA, 3, 0, 0, 0, 0x3B, 0xCA, 0x0F, 0x42, 0xD1 };
    std::vector<uint8_t> r8 { 0x41, 0xB8, 1, 0, 0, 0, 0x41, 0x3B, 0xC8, 0x44, 0x0F, 0x42, 0xC1 };
    std::vector<Section> sections { { edx.data(), edx.size(), true } };
    auto match = Scan(sections);
    assert(match.matches == 1 && match.Accepted() && match.maximum == 3);
    assert(match.countAt == (uintptr_t) edx.data() + 1);
    sections.push_back({ r8.data(), r8.size(), false });
    assert(Scan(sections).Accepted()); // data sections never count
    sections.back().executable = true;
    assert(Scan(sections).matches == 2 && !Scan(sections).Accepted());
    r8[2] = 4;
    assert(Scan(sections).matches == 2 && !Scan(sections).Accepted()); // unsupported second min still counts
    r8[2] = 1;
    sections.erase(sections.begin());
    assert(Scan(sections).Accepted() && Scan(sections).maximum == 1);
    for (uint8_t bad : { 0, 2, 4, 6, 255 })
    {
        r8[2] = bad;
        assert(!Scan(sections).Accepted());
    }
    r8[2] = 5;
    assert(Scan(sections).Accepted());
    r8[3] = 1; // full DWORD, not only its low byte
    assert(!Scan(sections).Accepted());
    r8[3] = 0;
    for (size_t n = 0; n < r8.size(); ++n)
        assert(Scan(std::vector<Section> { { r8.data(), n, true } }).matches == 0);
    assert(Scan(std::vector<Section> {}).matches == 0);
    std::vector<uint8_t> noMatch(64, 0x90);
    assert(Scan(std::vector<Section> { { noMatch.data(), noMatch.size(), true } }).matches == 0);
    edx.back() = 0xD2; // foreign cmovb source rewrite
    assert(Scan(std::vector<Section> { { edx.data(), edx.size(), true } }).matches == 0);
    edx.back() = 0xD1;

    assert(LegacyInterposer(true, false));
    assert(!LegacyInterposer(true, true) && !LegacyInterposer(false, false));

    Memory mem;
    Attempts attempts;
    sections = { { edx.data(), edx.size(), true } };
    assert(attempts.NeedsAttempt(1, false, sections));
    std::vector<MfgPatch::Site> sites;
    const uint8_t five = 5;
    assert(MfgPatch::AddSite(sites, mem, (uintptr_t) edx.data() + 1, &five, 1));
    assert(MfgPatch::Commit(sites, mem) == MfgPatch::Outcome::Committed);
    attempts.Remember(1, sites);
    assert(!attempts.NeedsAttempt(1, true, sections));
    edx[1] = 3; // new image at exactly the same base
    assert(attempts.NeedsAttempt(1, true, sections));
    assert(!attempts.NeedsAttempt(1, false, sections)); // hot path does not rescan
    assert(attempts.NeedsAttempt(2, true, sections));   // new base / second copy
    attempts.Remember(2, {});                           // refused image logs once, independently of copy 1
    assert(!attempts.NeedsAttempt(2, true, sections));
    assert(attempts.NeedsAttempt(1, true, sections));
    assert(!attempts.NeedsAttempt(1, true, std::vector<Section> {})); // old address never read

    using namespace DlssgPresetPolicy;
    assert(Parse("AUTO") == Preset::Auto && Parse("a") == Preset::A && Parse("B") == Preset::B);
    assert(Parse("c") == Preset::C && !Parse("4") && !Parse("garbage"));
    assert(!Override(Preset::Auto, true, Version(310, 9, 16)));
    assert(!Override(Preset::A, false, Version(310, 9, 16)));
    assert(Override(Preset::A, true, 0) == 1 && Override(Preset::B, true, 0) == 2);
    assert(!Override(Preset::C, true, 0));
    assert(!Override(Preset::C, true, Version(310, 9, 15)));
    assert(Override(Preset::C, true, Version(310, 9, 16)) == 3);
    assert(Override(Preset::C, true, Version(310, 10, 0)) == 3);
    assert(!Override(static_cast<Preset>(99), true, Version(310, 9, 16)));
    assert(!Available(false, true, true) && !Available(true, false, true) && !Available(true, true, false));
    assert(Available(true, true, true));
    Setting setting {};
    setting.version = 7;
    setting.settingLocation = 9;
    setting.isCurrentPredefined = 1;
    setting.isPredefinedValid = 1;
    setting.u32CurrentValue = 99;
    setting.u32PredefinedValue = 77;
    const auto original = setting;
    assert(Answer(0, kSettingId, &setting, 7, std::nullopt) == 0);
    assert(std::memcmp(&setting, &original, sizeof(setting)) == 0);
    assert(Answer(0, kSettingId + 1, &setting, 7, 1) == 0 && setting.u32CurrentValue == 99);
    assert(Answer(-1, kSettingId, &setting, 7, 1) == -1 && setting.u32CurrentValue == 99);
    assert(Answer(0, kSettingId, &setting, 8, 1) == 0 && setting.u32CurrentValue == 99);
    setting.settingType = 1;
    assert(Answer(0, kSettingId, &setting, 7, 1) == 0 && setting.u32CurrentValue == 99);
    setting = original;
    assert(Answer(0, kSettingId, &setting, 7, 2) == 0 && setting.u32CurrentValue == 2);
    assert(setting.u32PredefinedValue == 77 && setting.isPredefinedValid == 1);
    assert(setting.settingLocation == 0 && setting.isCurrentPredefined == 0);
    setting = original;
    assert(Answer(-160, kSettingId, &setting, 7, 3) == 0 && setting.u32CurrentValue == 3);
    assert(setting.version == 7 && setting.settingId == kSettingId && setting.settingType == 0);
    assert(setting.isPredefinedValid == 0 && setting.u32PredefinedValue == 0);
    assert(Answer<Setting>(-160, kSettingId, nullptr, 7, 3) == -160);
    std::puts("MFG wrapper reload, unique scan and DRS preset policies: PASS");
}
