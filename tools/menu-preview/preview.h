// Included after the unchanged production menu body in the generated translation unit.
#include <menu/font/Hack_Compressed.h>
#include "raster.h"
#include <imgui/imgui_internal.h>

namespace Preview
{
void Off()
{
    Config::Instance()->DlssNrEnabled = false;
    DlssNr::running = false;
}
void Simple()
{
    Config::Instance()->DlssNrEnabled = true;
    Config::Instance()->DlssNrHookMethod = 1;
}
void Full()
{
    Simple();
    Config::Instance()->DlssNrShowAllSettings = true;
}
void Passes()
{
    Full();
    auto c = Config::Instance();
    c->SetDlssNrPassCount(3);
    c->SetDlssNrIndividualPassSettings(true);
    DlssNrPassSettings first;
    first.Intensity = 0.7f;
    first.Preset = 1;
    c->SetDlssNrPassOverrides(0, first);
    DlssNrPassSettings second;
    second.LocalStructure = 0.8f;
    c->SetDlssNrPassOverrides(1, second);
}
void Hdr()
{
    Full();
    Config::Instance()->DlssNrHookMethod = 2;
    Config::Instance()->DlssNrHdrPresent = true;
    DlssNr::hdr = { 203.0f, true, true, true, true, DlssNr::PresentColour::Hdr10, nullptr };
}
void Vulkan()
{
    Full();
    State::Instance().api = API::Vulkan;
    DlssNr::nativeVk = true;
}
void Missing()
{
    Simple();
    DlssNr::running = false;
    DlssNr::failure = "nvngx_dlssnr.dll was not found beside OptiScaler or the game";
}
void Compare()
{
    Full();
    auto c = Config::Instance();
    c->DlssNrHoldFrame = true;
    c->DlssNrCompare = 2;
    c->DlssNrCompareTags = true;
    c->DlssNrCompareSplit = 0.45f;
}
void AdvisorEligible()
{
    Simple();
    auto* c = Config::Instance();
    c->DlssNrWorkingScale = 1.0f;
    c->DlssNrCadence = 1;
    c->DlssNrCadenceAdaptive = false;
    c->DlssNrGpuTiming = true;
    c->DlssNrGpuTimingInterval = 1;
    auto& t = DlssNr::GpuTiming::snapshot;
    t.valid = t.enabled = true;
    t.sampleAgeMs = 0;
    t.metadata.stage = "after";
    t.metadata.workingScale = 1;
    t.metadata.evaluationId = 100;
    t.metadata.contractHash = 123;
    t.metadata.outputWidth = 1920;
    t.metadata.outputHeight = 1080;
}
void AdvisorWaiting()
{
    AdvisorEligible();
    Config::Instance()->DlssNrGpuTiming = false;
}
void AdvisorRunning()
{
    AdvisorEligible();
    DlssNr::AdvisorRuntime::Start({ 1, 1 }, 60);
    DlssNr::Advisor::Context c;
    c.device = 1;
    c.width = 1920;
    c.height = 1080;
    c.route = static_cast<uint32_t>(State::Instance().api) << 8 |
              (Config::Instance()->DlssNrHookMethod.value_or_default() << 16);
    DlssNr::AdvisorRuntime::Frame(c, 1, true);
}
void AdvisorResults(bool rr = false)
{
    AdvisorEligible();
    auto& s = DlssNr::AdvisorRuntime::session;
    s.Start({ 1, 1 }, 60, 0);
    DlssNr::Advisor::Context c;
    c.device = 1;
    c.width = 1920;
    c.height = 1080;
    c.route = static_cast<uint32_t>(State::Instance().api) << 8 |
              (Config::Instance()->DlssNrHookMethod.value_or_default() << 16);
    c.afterRR = rr;
    DlssNr::GpuTiming::snapshot.metadata.stage = rr ? "after-rr" : "after";
    s.Observe(c);
    double now = 0;
    uint64_t id = 0;
    while (s.Active())
    {
        auto token = s.Token();
        now += 1100;
        for (unsigned i = 0; i < 90; ++i)
        {
            const auto cadence = s.Override()->cadence;
            const bool carried = cadence > 1 && i % cadence != 0;
            const double base = s.Index() == 0 ? 24.0 : 16.0;
            s.Accept({ token, ++id, 123, carried ? 1.0 : 6.0, base, true, false, false, carried }, now);
        }
    }
    s.Restored();
    DlssNr::AdvisorRuntime::Publish();
}
void AdvisorDone() { AdvisorResults(); }
void AdvisorRRDone() { AdvisorResults(true); }
struct Scene
{
    const char* name;
    void (*setup)();
    bool portuguese;
    int height;
};
const Scene scenes[] = { { "nr-off", Off, false, 560 },
                         { "d3d12-simple", Simple, false, 560 },
                         { "d3d12-full", Full, false, 2600 },
                         { "individual-passes", Passes, false, 2600 },
                         { "present-hdr", Hdr, false, 2600 },
                         { "native-vulkan", Vulkan, false, 2600 },
                         { "model-missing", Missing, false, 560 },
                         { "comparison", Compare, false, 2600 },
                         { "d3d12-simple-pt-BR", Simple, true, 560 },
                         { "present-hdr-pt-BR", Hdr, true, 2600 },
                         { "advisor-eligible-pt-BR", AdvisorEligible, true, 1000 },
                         { "advisor-running-pt-BR", AdvisorRunning, true, 1000 },
                         { "advisor-done-pt-BR", AdvisorDone, true, 1300 },
                         { "advisor-rr-done-pt-BR", AdvisorRRDone, true, 1300 },
                         { "advisor-timing-off-pt-BR", AdvisorWaiting, true, 1000 },
                         { "advisor-hidden-off", Off, true, 560 } };
} // namespace Preview
int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--list")
        {
            for (const auto& s : Preview::scenes)
                std::cout << s.name << '\n';
            return 0;
        }
        if (argc != 4)
            throw std::runtime_error("usage: menu-preview STATE OUTPUT_DIRECTORY REPO_ROOT");
        const Preview::Scene* scene = nullptr;
        for (const auto& s : Preview::scenes)
            if (s.name == std::string(argv[1]))
                scene = &s;
        if (!scene)
            throw std::runtime_error("Unknown preview state");
        const std::filesystem::path output(argv[2]), root(argv[3]);
        if (scene->portuguese)
        {
            std::ifstream pack(root / "retoque/translations/optional/pt-BR.lang");
            if (!pack || !Localization::dictionary.Load(pack))
                throw std::runtime_error("Invalid Portuguese pack");
        }
        scene->setup();
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(560, float(scene->height));
        io.DeltaTime = 1.0f / 60.0f;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
        if (Config::Instance()->UseHQFont.value_or_default())
            io.FontDefault = io.Fonts->AddFontFromMemoryCompressedBase85TTF(
                hack_compressed_compressed_data_base85, Config::Instance()->FontSize.value_or_default());
        else
            io.FontDefault = io.Fonts->AddFontDefault();
        ImGui::StyleColorsDark();
        MenuCommon::ApplyThemeStyle();
        std::string text;
        float contentBottom = 0;
        // Child auto-height and clipping converge over frames, just as they do in the real menu.
        for (int frame = 0; frame < 5; ++frame)
        {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(12, 12));
            ImGui::SetNextWindowSize(ImVec2(536, float(scene->height) - 24));
            ImGui::Begin("Retoque | synthetic preview", nullptr,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
            ImGui::TextDisabled("PREVIEW | synthetic values | no game or GPU");
            ImGui::Separator();
            if (frame == 4)
                ImGui::LogToBuffer();
            DlssNr::RenderMenu(Config::Instance(), 1.0f);
            contentBottom = ImGui::GetCursorPosY();
            if (frame == 4)
            {
                text = GImGui->LogBuffer.c_str();
                ImGui::LogFinish();
            }
            ImGui::End();
            ImGui::Render();
            for (auto* texture : *ImGui::GetDrawData()->Textures)
            {
                if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates)
                {
                    texture->SetTexID(1);
                    texture->SetStatus(ImTextureStatus_OK);
                }
                else if (texture->Status == ImTextureStatus_WantDestroy)
                {
                    texture->SetTexID(ImTextureID_Invalid);
                    texture->SetStatus(ImTextureStatus_Destroyed);
                }
            }
        }
        Preview::Raster raster(560, scene->height);
        raster.Draw(*ImGui::GetDrawData());
        std::filesystem::create_directories(output);
        const auto stem = output / scene->name;
        raster.Save(stem.string() + ".png");
        std::ofstream(stem.string() + ".txt") << text;
        std::ofstream(stem.string() + ".json")
            << "{\"width\":560,\"height\":" << scene->height << ",\"triangles\":" << raster.triangles
            << ",\"glyph_pixels\":" << raster.glyphPixels << ",\"content_bottom\":" << contentBottom << "}\n";
        ImGui::DestroyContext();
        std::cout << scene->name << ": " << raster.triangles << " triangles, " << raster.glyphPixels
                  << " glyph pixels\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
