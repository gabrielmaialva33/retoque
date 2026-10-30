// Appended by run.py after fakes and the block sliced from dllmain.cpp (RunUnrealSearch)

static int wiringFailures = 0;

static void Check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++wiringFailures;
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    namespace fs = std::filesystem;
    const fs::path root = fs::path(argv[1]) / "wiring";
    const fs::path exe = root / "Game/Binaries/Win64/";
    fs::create_directories(exe);
    for (auto [plugin, name] : { std::pair { "DLSS", "nvngx_dlss.dll" }, std::pair { "DLSS", "nvngx_dlssd.dll" },
                                 std::pair { "StreamlineCore", "nvngx_dlssg.dll" } })
    {
        const auto folder = root / "Engine/Plugins/Runtime/Nvidia" / plugin / "Binaries/ThirdParty/Win64";
        fs::create_directories(folder);
        std::ofstream(folder / name) << "x";
    }

    auto& state = State::Instance();
    state.NVNGX_DLSS_Path = L"C:\\game\\kept\\nvngx_dlss.dll";
    RunUnrealSearch(exe);
    Check(state.NVNGX_DLSS_Path == std::wstring(L"C:\\game\\kept\\nvngx_dlss.dll"),
          "a file the existing searches found is kept unchanged");
    Check(state.NVNGX_DLSSD_Path.has_value() &&
              fs::path(*state.NVNGX_DLSSD_Path) ==
                  root / "Engine/Plugins/Runtime/Nvidia/DLSS/Binaries/ThirdParty/Win64/nvngx_dlssd.dll",
          "a missing nvngx_dlssd.dll is filled with the file itself");
    Check(state.NVNGX_DLSSG_Path.has_value() &&
              fs::path(*state.NVNGX_DLSSG_Path).parent_path() ==
                  root / "Engine/Plugins/Runtime/Nvidia/StreamlineCore/Binaries/ThirdParty/Win64",
          "a missing nvngx_dlssg.dll is filled; its folder is what NGX gets");
    Check(logged == 2, "each file filled is logged once");

    state = {};
    logged = 0;
    RunUnrealSearch(fs::path(argv[1]) / "no-engine/");
    Check(!state.NVNGX_DLSS_Path && !state.NVNGX_DLSSD_Path && !state.NVNGX_DLSSG_Path && logged == 0,
          "no Unreal layout: nothing filled, DLSS stays off as before");

    std::printf("%s\n", wiringFailures ? "FAILED" : "all passed");
    return wiringFailures ? 1 : 0;
}
