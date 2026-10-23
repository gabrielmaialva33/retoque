#define main original_render_size_cases
#include "../nr-render-size/cases.cpp"
#undef main
#include <algorithm>
#include <cstdint>
#include <vector>
#if __has_include(<dlssnr/DlssNr_RenderGeometry.h>)
#include <dlssnr/DlssNr_RenderGeometry.h>
#else
// Baseline adapter: old GetRenderSize has no feature registration/evaluate binding.
// Keep testing the actual old resolver, rather than a replacement geometry algorithm.
namespace DlssNr::RenderGeometry
{
struct Creation
{
};
Creation ReadCreation(const NVSDK_NGX_Parameter*) { return {}; }
void Remember(uint64_t, Creation) {}
void Forget(uint64_t) {}
struct Scope
{
    Scope(bool, uint64_t, const NVSDK_NGX_Parameter*) {}
};
} // namespace DlssNr::RenderGeometry
#endif

static void same(DlssNr::RenderSize size, unsigned width, unsigned height)
{
    assert(size.width == width && size.height == height);
}
int main()
{
    using namespace DlssNr::RenderGeometry;
    unsigned checks = 0;
    FakeParams creation, empty;
    creation.Put(NVSDK_NGX_Parameter_Width, 32);
    creation.Put(NVSDK_NGX_Parameter_Height, 18);
    creation.Put(NVSDK_NGX_Parameter_OutWidth, 64);
    creation.Put(NVSDK_NGX_Parameter_OutHeight, 36);
    const auto initial = ReadCreation(&creation);
    Remember(17, initial);
    {
        Scope evaluation(true, 17, &empty);
        const auto size = DlssNr::GetRenderSize(&empty);
        same(size, 32, 18);
        // A 96x72 allocation contains valid data only in the creation rectangle.
        std::vector<int> plane(96 * 72, -9999);
        for (unsigned y = 0; y < 18; ++y)
            for (unsigned x = 0; x < 32; ++x)
                plane[y * 96 + x] = 1;
        int sum = 0;
        for (unsigned y = 0; y < size.height; ++y)
            for (unsigned x = 0; x < size.width; ++x)
                sum += plane[y * 96 + x];
        assert(sum == 576);
        ++checks;
        FakeParams foreign;
        same(DlssNr::GetRenderSize(&foreign), 0, 0);
        ++checks;
        {
            Scope disabled(false, 17, &foreign);
            same(DlssNr::GetRenderSize(&foreign), 0, 0);
        }
        same(DlssNr::GetRenderSize(&empty), 32, 18);
        ++checks;
    }
    same(DlssNr::GetRenderSize(&empty), 0, 0);
    ++checks;
    FakeParams incomplete;
    incomplete.Put(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 24);
    {
        Scope evaluation(true, 17, &incomplete);
        same(DlssNr::GetRenderSize(&incomplete), 24, 0);
    }
    ++checks;
    FakeParams drs;
    drs.Put(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 24);
    drs.Put(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 14);
    {
        Scope evaluation(true, 17, &drs);
        same(DlssNr::GetRenderSize(&drs), 24, 14);
    }
    {
        Scope evaluation(true, 17, &empty);
        same(DlssNr::GetRenderSize(&empty), 0, 0);
    }
    ++checks;
    drs.Put(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 32);
    drs.Put(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 18);
    {
        Scope evaluation(true, 17, &drs);
        same(DlssNr::GetRenderSize(&drs), 32, 18);
    }
    {
        Scope evaluation(true, 17, &empty);
        same(DlssNr::GetRenderSize(&empty), 0, 0);
    }
    ++checks;
    Remember(17, initial);
    {
        Scope evaluation(true, 17, &empty);
        same(DlssNr::GetRenderSize(&empty), 32, 18);
    }
    ++checks;
    FakeParams changedDisplay;
    changedDisplay.Put(NVSDK_NGX_Parameter_OutWidth, 80);
    changedDisplay.Put(NVSDK_NGX_Parameter_OutHeight, 45);
    {
        Scope evaluation(true, 17, &changedDisplay);
        same(DlssNr::GetRenderSize(&changedDisplay), 0, 0);
    }
    {
        Scope evaluation(true, 17, &empty);
        same(DlssNr::GetRenderSize(&empty), 0, 0);
    }
    ++checks;
    Remember(17, initial);
    {
        Scope disabled(false, 17, &empty);
        same(DlssNr::GetRenderSize(&empty), 0, 0);
    }
    ++checks;
    {
        Scope outer(true, 17, &empty);
        FakeParams another;
        Remember(18, ReadCreation(&creation));
        {
            Scope inner(true, 18, &another);
            same(DlssNr::GetRenderSize(&another), 32, 18);
        }
        same(DlssNr::GetRenderSize(&empty), 32, 18);
    }
    ++checks;
    Forget(17);
    {
        Scope evaluation(true, 17, &empty);
        same(DlssNr::GetRenderSize(&empty), 0, 0);
    }
    ++checks;
    FakeParams oneToOne;
    oneToOne.Put(NVSDK_NGX_Parameter_Width, 64);
    oneToOne.Put(NVSDK_NGX_Parameter_Height, 36);
    oneToOne.Put(NVSDK_NGX_Parameter_OutWidth, 64);
    oneToOne.Put(NVSDK_NGX_Parameter_OutHeight, 36);
    same(DlssNr::GetRenderSize(&oneToOne), 64, 36);
    ++checks;
    oneToOne.values.erase(NVSDK_NGX_Parameter_OutWidth);
    same(DlssNr::GetRenderSize(&oneToOne), 64, 36);
    ++checks;
    // Backends may update the same block before the evaluate unwinds.
    Remember(17, initial);
    {
        Scope evaluation(true, 17, &empty);
        empty.Put(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 24);
        empty.Put(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 14);
        same(DlssNr::GetRenderSize(&empty), 24, 14);
    }
    empty.values.clear();
    {
        Scope evaluation(true, 17, &empty);
        same(DlssNr::GetRenderSize(&empty), 0, 0);
    }
    ++checks;
    // Invalid/recreated (including an NR-off creation) never inherits the previous epoch.
    Remember(17, initial);
    Remember(17, {});
    {
        Scope evaluation(true, 17, &empty);
        same(DlssNr::GetRenderSize(&empty), 0, 0);
    }
    ++checks;
    std::printf("PASS: %u feature-scoped render geometry cases\n", checks);
}
