#include <misc/UnrealDlssSearch.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;

static void Expect(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

static fs::path Touch(const fs::path& file)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file) << "x";
    return file;
}

static bool Is(const std::optional<fs::path>& found, const fs::path& expected)
{
    if (!found.has_value())
        return false;
    if (found->lexically_normal() == expected.lexically_normal())
        return true;
    std::printf("  got %s\n", found->string().c_str());
    return false;
}

static const fs::path ThirdParty = "Binaries/ThirdParty/Win64";

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    const fs::path base = fs::path(argv[1]);
    using UnrealDlssSearch::Find;

    // 1. Packaged UE game, engine plugin: <Root>/<Project>/Binaries/Win64 with <Root>/Engine
    {
        const auto root = base / "standard";
        const auto exe = root / "Game/Binaries/Win64";
        fs::create_directories(exe);
        const auto sr = Touch(root / "Engine/Plugins/Runtime/Nvidia/DLSS" / ThirdParty / "nvngx_dlss.dll");
        const auto rr = Touch(root / "Engine/Plugins/Runtime/Nvidia/DLSS" / ThirdParty / "nvngx_dlssd.dll");
        const auto fg = Touch(root / "Engine/Plugins/Runtime/Nvidia/StreamlineCore" / ThirdParty / "nvngx_dlssg.dll");
        Expect(Is(Find(exe, "nvngx_dlss.dll"), sr), "engine DLSS plugin: nvngx_dlss.dll");
        Expect(Is(Find(exe / "", "nvngx_dlss.dll"), sr), "trailing separator (remove_filename) finds the same file");
        Expect(Is(Find(exe, "nvngx_dlssd.dll"), rr), "engine DLSS plugin: nvngx_dlssd.dll");
        Expect(Is(Find(exe, "nvngx_dlssg.dll"), fg), "engine StreamlineCore plugin: nvngx_dlssg.dll");
        Expect(!Find(exe, "nvngx_dlssnr.dll").has_value(), "a file in no candidate folder is not found");
    }

    // 2. Phantom Blade Zero shape: exe outside Binaries (<Root>/platform/steam), plugins in <Root>/Engine
    {
        const auto root = base / "pbz";
        const auto exe = root / "platform/steam";
        fs::create_directories(exe);
        const auto sr = Touch(root / "Engine/Plugins/Runtime/Nvidia/DLSS" / ThirdParty / "nvngx_dlss.dll");
        Expect(Is(Find(exe / "", "nvngx_dlss.dll"), sr), "exe outside Binaries reaches the Engine root");
    }

    // 3. Project plugin over engine plugin; plugin containers in fixed order
    {
        const auto root = base / "project";
        const auto exe = root / "Game/Binaries/Win64";
        fs::create_directories(exe);
        Touch(root / "Engine/Plugins/Runtime/Nvidia/DLSS" / ThirdParty / "nvngx_dlss.dll");
        const auto project = Touch(root / "Game/Plugins/DLSS" / ThirdParty / "nvngx_dlss.dll");
        Expect(Is(Find(exe, "nvngx_dlss.dll"), project), "the exe's project plugin precedes the engine plugin");
        const auto nested = Touch(root / "Game/Plugins/Runtime/Nvidia/DLSS" / ThirdParty / "nvngx_dlss.dll");
        Expect(Is(Find(exe, "nvngx_dlss.dll"), nested), "Plugins/Runtime/Nvidia precedes Plugins");
        const auto rr = Touch(root / "Game/Plugins/Marketplace/DLSS" / ThirdParty / "nvngx_dlssd.dll");
        Expect(Is(Find(exe, "nvngx_dlssd.dll"), rr), "Marketplace plugin folder");
        const auto ngx = Touch(root / "Game/Binaries/ThirdParty/NVIDIA/NGX/Win64/nvngx_dlssg.dll");
        Expect(Is(Find(exe, "nvngx_dlssg.dll"), ngx), "project NGX third-party folder");
    }

    // 4. DLSS* / Streamline* plugin names: sorted, first match; other plugin names ignored
    {
        const auto root = base / "names";
        const auto exe = root / "Game/Binaries/Win64";
        fs::create_directories(exe);
        const auto first = Touch(root / "Engine/Plugins/Runtime/Nvidia/DLSS" / ThirdParty / "nvngx_dlss.dll");
        Touch(root / "Engine/Plugins/Runtime/Nvidia/DLSS_Legacy" / ThirdParty / "nvngx_dlss.dll");
        Expect(Is(Find(exe, "nvngx_dlss.dll"), first), "DLSS sorts before DLSS_Legacy");
        const auto renamed = Touch(root / "Engine/Plugins/Runtime/Nvidia/DLSSPlugin" / ThirdParty / "nvngx_dlssd.dll");
        Expect(Is(Find(exe, "nvngx_dlssd.dll"), renamed), "a DLSS-prefixed plugin name is a candidate");
        const auto sl = Touch(root / "Engine/Plugins/Runtime/Nvidia/Streamline" / ThirdParty / "nvngx_dlssg.dll");
        Expect(Is(Find(exe, "nvngx_dlssg.dll"), sl), "the pre-5.2 Streamline plugin name is a candidate");
        Touch(root / "Engine/Plugins/Runtime/Nvidia/Other" / ThirdParty / "sl.interposer.dll");
        Touch(root / "Engine/Plugins/Runtime/Nvidia/NotDLSS" / ThirdParty / "nvngx_dlssnr.dll");
        Expect(!Find(exe, "nvngx_dlssnr.dll").has_value(), "a plugin not named DLSS*/Streamline* is not searched");
    }

    // 5. No recursion: a file below or beside a fixed folder is not found
    {
        const auto root = base / "bounded";
        const auto exe = root / "Game/Binaries/Win64";
        fs::create_directories(exe);
        Touch(root / "Engine/Plugins/Runtime/Nvidia/DLSS" / ThirdParty / "old/nvngx_dlss.dll");
        Touch(root / "Engine/Plugins/Some/Deep/DLSS" / ThirdParty / "nvngx_dlssd.dll");
        Touch(root / "Engine/Plugins/Runtime/Nvidia/DLSS/Binaries/nvngx_dlssg.dll");
        Expect(!Find(exe, "nvngx_dlss.dll").has_value(), "below the Win64 folder is not searched");
        Expect(!Find(exe, "nvngx_dlssd.dll").has_value(), "an unknown plugin container is not searched");
        Expect(!Find(exe, "nvngx_dlssg.dll").has_value(), "outside ThirdParty/Win64 is not searched");
        fs::create_directories(root / "Engine/Plugins/DLSS" / ThirdParty / "nvngx_dlss.dll");
        Expect(!Find(exe, "nvngx_dlss.dll").has_value(), "a folder named like the file is not a match");
    }

    // 6. The ascent is bounded and the nearest Engine wins
    {
        const auto far = base / "far";
        const auto exe = far / "a/b/c/d/e";
        fs::create_directories(exe);
        Touch(far / "Engine/Plugins/Runtime/Nvidia/DLSS" / ThirdParty / "nvngx_dlss.dll");
        Expect(!Find(exe, "nvngx_dlss.dll").has_value(), "an Engine five folders up is beyond the bound");
        const auto near = Touch(far / "a/Engine/Plugins/DLSS" / ThirdParty / "nvngx_dlss.dll");
        Expect(Is(Find(exe, "nvngx_dlss.dll"), near), "an Engine four folders up is within the bound");
        Expect(Is(Find(exe / "", "nvngx_dlss.dll"), near), "trailing separator is not an extra level");
        fs::create_directories(far / "a/b/Engine");
        Expect(!Find(exe, "nvngx_dlss.dll").has_value(), "the nearest Engine root is the only one searched");

        const auto file = base / "engine-file";
        fs::create_directories(file / "Game/Binaries/Win64");
        Touch(file / "Engine");
        Expect(UnrealDlssSearch::CandidateFolders(file / "Game/Binaries/Win64").empty(),
               "a file named Engine is not an Engine root");
        Expect(UnrealDlssSearch::CandidateFolders(base / "none/x").empty(), "no Engine root: no candidates");
        Expect(!Find(fs::path("/"), "nvngx_dlss.dll").has_value(), "the filesystem root ends the ascent");
        Expect(!Find(fs::path(), "nvngx_dlss.dll").has_value(), "an empty path is a harmless miss");
    }

    // 7. Exe outside every project: the project is used only when it is the root's single one
    {
        const auto root = base / "single";
        const auto exe = root / "platform/steam";
        fs::create_directories(exe);
        fs::create_directories(root / "Engine/Binaries");
        const auto only = Touch(root / "Only/Plugins/DLSS" / ThirdParty / "nvngx_dlss.dll");
        fs::create_directories(root / "Only/Binaries/Win64");
        Expect(Is(Find(exe, "nvngx_dlss.dll"), only), "the root's single project is searched");

        const auto two = base / "two";
        fs::create_directories(two / "platform/steam");
        fs::create_directories(two / "Engine");
        fs::create_directories(two / "First/Binaries");
        fs::create_directories(two / "Second/Binaries");
        Touch(two / "First/Plugins/DLSS" / ThirdParty / "nvngx_dlss.dll");
        Expect(!Find(two / "platform/steam", "nvngx_dlss.dll").has_value(),
               "two projects: neither project's plugins are guessed");
        const auto engine = Touch(two / "Engine/Plugins/DLSS" / ThirdParty / "nvngx_dlss.dll");
        Expect(Is(Find(two / "platform/steam", "nvngx_dlss.dll"), engine),
               "two projects: the engine is still searched");

        // The exe's own project is used even when the root has other projects
        Touch(two / "Second/Plugins/DLSS" / ThirdParty / "nvngx_dlssd.dll");
        fs::create_directories(two / "Second/Binaries/Win64");
        const auto mine = Touch(two / "First/Plugins/DLSS" / ThirdParty / "nvngx_dlssd.dll");
        Expect(Is(Find(two / "First/Binaries/Win64", "nvngx_dlssd.dll"), mine), "the exe's project, not a sibling");
        Expect(Is(Find(two / "Second/Binaries/Win64", "nvngx_dlssd.dll"),
                  two / "Second/Plugins/DLSS" / ThirdParty / "nvngx_dlssd.dll"),
               "the other exe's project");
    }

    // 8. An unreadable folder is skipped, not thrown through
    {
        const auto root = base / "denied";
        const auto exe = root / "Game/Binaries/Win64";
        fs::create_directories(exe);
        Touch(root / "Engine/Plugins/Runtime/Nvidia/DLSS" / ThirdParty / "nvngx_dlss.dll");
        const auto fallback = Touch(root / "Engine/Plugins/DLSS" / ThirdParty / "nvngx_dlss.dll");
        fs::permissions(root / "Engine/Plugins/Runtime/Nvidia", fs::perms::none);
        bool threw = false;
        std::optional<fs::path> found;
        try
        {
            found = Find(exe, "nvngx_dlss.dll");
        }
        catch (...)
        {
            threw = true;
        }
        fs::permissions(root / "Engine/Plugins/Runtime/Nvidia", fs::perms::owner_all);
        Expect(!threw && Is(found, fallback), "an unreadable plugin container is skipped");
    }

    // 9. The candidate list stays small and in a fixed order
    {
        const auto exe = base / "standard/Game/Binaries/Win64";
        const auto first = UnrealDlssSearch::CandidateFolders(exe);
        const auto second = UnrealDlssSearch::CandidateFolders(exe);
        Expect(first == second, "the same tree gives the same candidates");
        Expect(!first.empty() && first.size() <= 64, "a bounded number of candidates");
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
