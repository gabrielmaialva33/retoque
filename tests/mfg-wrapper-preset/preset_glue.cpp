#include "DlssgPresetDrs.h"
#include "DlssgPresetMenu.h"
#include <cassert>
#include <cstdio>
uint64_t runtime = 0;
bool observed = false;
uint64_t MfgUnlock::PresetRuntimeVersion() { return runtime; }
bool MfgUnlock::PresetProviderObserved() { return observed; }
int main(int argc, char** argv)
{
    using namespace DlssgPresetPolicy;
    assert(argc == 2);
    Config::Instance()->FGDLSSGAdaMfgPreset = std::string(argv[1]);
    const auto selected = Parse(argv[1]).value();
    Config::Instance()->FGDLSSGAdaMfgUnlock = true;
    NVDRS_SETTING setting {};
    setting.version = NVDRS_SETTING_VER;
    setting.u32CurrentValue = 99;
    assert(AnswerRead(NVAPI_OK, &setting) == NVAPI_OK);
    assert(setting.u32CurrentValue == (selected == Preset::A ? 1u : 99u));
    runtime = Version(310, 9, 16);
    assert(AnswerRead(NVAPI_OK, &setting) == NVAPI_OK);
    assert(setting.u32CurrentValue == (selected == Preset::Auto ? 99u : static_cast<unsigned>(selected)));
    Config::Instance()->FGDLSSGAdaMfgPreset = std::string("b");
    AnswerRead(NVAPI_OK, &setting);
    assert(setting.u32CurrentValue == (selected == Preset::Auto ? 99u : static_cast<unsigned>(selected)));
    Config::Instance()->FGDLSSGAdaMfgUnlock = false;
    setting.u32CurrentValue = 98;
    AnswerRead(NVAPI_OK, &setting);
    assert(setting.u32CurrentValue == 98);
    RenderMenu(true);
    assert(ImGui::combos == 0);
    Config::Instance()->FGDLSSGAdaMfgUnlock = true;
    RenderMenu(true);
    assert(ImGui::combos == 0); // no DRS
    NvApiHooks::o_NvAPI_DRS_GetSetting = (void*) 1;
    RenderMenu(true);
    assert(ImGui::combos == 0); // no observed provider
    observed = true;
    RenderMenu(false);
    assert(ImGui::combos == 0); // replacement route
    runtime = 0;
    RenderMenu(true);
    assert(ImGui::combos == 1 && ImGui::cDisabled);
    runtime = Version(310, 9, 15);
    RenderMenu(true);
    assert(ImGui::combos == 2 && ImGui::cDisabled);
    runtime = Version(310, 9, 16);
    ImGui::choose = 3;
    RenderMenu(true);
    assert(ImGui::combos == 3 && !ImGui::cDisabled);
    assert(Config::Instance()->FGDLSSGAdaMfgPreset.value == "c");
    std::puts("Production DRS adapter/session latch and menu availability: PASS");
}
