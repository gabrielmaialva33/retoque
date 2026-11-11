#include "pch.h"
#include "MenuFonts.h"
#include "Localization.h"

#include <imgui/imgui.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace MenuFonts
{
// Marks the merged source so a menu re-init (the atlas survives it) does not add it twice.
static constexpr char kSourceName[] = "Retoque menu language fallback";

void AddTranslationFallback(ImGuiIO& io)
{
    struct Decision
    {
        Script script = Script::None;
        std::vector<ImWchar> ranges; // Must outlive the atlas source that points at it.
    };
    static const Decision decision = []
    {
        Plan plan;
        Localization::Active().ForEachTranslation([&](std::string_view text) { plan.Add(text); });
        Decision made;
        made.script = plan.Needed();
        for (const auto c : plan.Ranges())
            made.ranges.push_back(static_cast<ImWchar>(c));
        return made;
    }();

    // English, pt-BR and every Latin or Cyrillic pack stop here: the atlas is untouched.
    if (decision.script == Script::None || decision.ranges.empty())
        return;

    ImFontAtlas* atlas = io.Fonts;
    if (!atlas->Fonts.empty())
        for (const ImFontConfig* source : atlas->Fonts[0]->Sources)
            if (std::strcmp(source->Name, kSourceName) == 0)
                return;

    // One warning per process: the menu may initialise again on the same atlas.
    static std::atomic_bool warned = false;
    static const std::filesystem::path fontsDir = []
    {
        wchar_t windowsDir[MAX_PATH] {};
        GetWindowsDirectoryW(windowsDir, MAX_PATH);
        return std::filesystem::path(windowsDir) / L"Fonts";
    }();
    const FontFile* chosen = Choose(decision.script,
                                    [](const wchar_t* name)
                                    {
                                        std::error_code ec;
                                        return std::filesystem::is_regular_file(fontsDir / name, ec);
                                    });
    if (chosen == nullptr)
    {
        if (!warned.exchange(true))
            LOG_WARN("Menu localization: OptiScaler.lang needs a {} font and none was found in {}; "
                     "those characters show as '?'",
                     ScriptName(decision.script), wstring_to_string(fontsDir.wstring()));
        return;
    }

    // ImGui adds this default itself on the first build when the menu added no font (UseHQFont=false).
    if (atlas->Fonts.empty())
        atlas->AddFontDefault();
    // MergeMode always appends to the last font; only the menu's own font may receive the glyphs.
    ImFont* target = atlas->Fonts[0];
    if (target != atlas->Fonts.back() || target->Sources.empty())
    {
        if (!warned.exchange(true))
            LOG_WARN("Menu localization: the menu font is not the atlas's last font; not merging the {} font",
                     ScriptName(decision.script));
        return;
    }

    ImFontConfig config;
    config.MergeMode = true;
    config.FontNo = chosen->faceIndex;
    // The same size as the menu font's first source, so the merged face is not scaled relative to it.
    const float size = target->Sources[0]->SizePixels;
    std::snprintf(config.Name, sizeof(config.Name), "%s", kSourceName);
    const auto path = wstring_to_string((fontsDir / chosen->name).wstring());
    if (atlas->AddFontFromFileTTF(path.c_str(), size, &config, decision.ranges.data()) == nullptr)
    {
        if (!warned.exchange(true))
            LOG_WARN("Menu localization: could not load {} for the {} menu text", path, ScriptName(decision.script));
    }
    else
        LOG_INFO("Menu localization: merged {} (face {}) into the menu font for {} text, {} glyph ranges", path,
                 chosen->faceIndex, ScriptName(decision.script), decision.ranges.size() / 2);
}
} // namespace MenuFonts
