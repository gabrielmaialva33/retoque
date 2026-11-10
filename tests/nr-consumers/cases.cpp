#include "DlssNr_ConsumerMarkers.h"

#include <cstdio>
#include <cstring>

using DlssNr::Consumers::MatchModuleName;

static int failures = 0;

static void Expect(const wchar_t* name, const char* project)
{
    const auto* found = MatchModuleName(name);
    const char* got = found == nullptr ? nullptr : found->project;

    if ((got == nullptr) != (project == nullptr) || (got != nullptr && std::strcmp(got, project) != 0))
    {
        std::fprintf(stderr, "FAIL %ls: expected %s, got %s\n", name, project ? project : "(none)",
                     got ? got : "(none)");
        ++failures;
    }
}

int main()
{
    const char* renodxDlss = "RenoDX DLSS (renodx-dlss / renodx-dlss5)";

    // Neural producers, any case.
    Expect(L"renodx-dlss5.addon64", renodxDlss);
    Expect(L"RenoDX-DLSS5.addon64", renodxDlss);
    Expect(L"renodx-dlss.addon64", renodxDlss);
    Expect(L"renodx_dlss5.addon64", renodxDlss);
    Expect(L"deep-fried-chicken.addon64", "Deep Fried Chicken");
    Expect(L"DLSS5-Feeder.dll", "DLSS5-Feeder");
    Expect(L"dlss5_feeder.asi", "DLSS5-Feeder");
    Expect(L"nr_native_adapter.dll", "AMDLSS5");
    Expect(L"NR_Native_Adapter.dll", "AMDLSS5");
    Expect(L"amdlss5.dll", "AMDLSS5");
    Expect(L"AMDLSS5_engine.dll", "AMDLSS5");

    // RenoDX's per-game HDR add-ons are not producers: combining one with our NR is the normal case.
    Expect(L"renodx-cyberpunk2077.addon64", nullptr);
    Expect(L"renodx-unrealengine.addon64", nullptr);
    Expect(L"renodx.addon64", nullptr);
    Expect(L"renodx-dl.addon64", nullptr);

    // Everything that legitimately shares the process with our NR.
    Expect(L"ReShade64.dll", nullptr);
    Expect(L"dxgi.dll", nullptr);
    Expect(L"OptiScaler.dll", nullptr);
    Expect(L"_nvngx.dll", nullptr);
    Expect(L"nvngx.dll", nullptr);
    Expect(L"nvngx_dlss.dll", nullptr);
    Expect(L"nvngx_dlssd.dll", nullptr);
    Expect(L"nvngx_dlssg.dll", nullptr);
    Expect(L"nvngx.dll_dlssnr.dll", nullptr);
    Expect(L"nvngx_dlssnr.dll", nullptr);
    Expect(L"sl.dlss_g.dll", nullptr);

    // AMDLSS5's other files: a generic proxy name, a host name that could be a substring of someone else's, and
    // the network it shares with us.
    Expect(L"version.dll", nullptr);
    Expect(L"nr_host.dll", nullptr);
    Expect(L"nvngx_dlssnr_host.dll", nullptr);
    Expect(L"amd_fidelityfx_upscaler_dx12.dll", nullptr);
    Expect(L"", nullptr);

    if (failures != 0)
        return 1;

    std::puts("PASS: producer markers match the neural add-ons and nothing that shares the process with our NR");
    return 0;
}
