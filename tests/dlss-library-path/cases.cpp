static int failures = 0;

static void Expect(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

// The folder the two users hand NGX: NVNGXProxy::GetFeatureCommonInfo (remove_filename) and the NVNGX inputs'
// override entry (parent_path)
static void ExpectFolder(const std::filesystem::path& folder, const std::string& what)
{
    auto& kept = State::Instance().NVNGX_DLSS_Path;
    Expect(kept.has_value(), what + ": the override is taken");
    if (!kept.has_value())
        return;

    std::filesystem::path path(*kept);
    Expect(path.filename() == "nvngx_dlss.dll", what + ": the kept path names the file");
    Expect(std::filesystem::equivalent(std::filesystem::path(path).remove_filename(), folder),
           what + ": remove_filename gives the folder");
    Expect(std::filesystem::equivalent(path.parent_path(), folder), what + ": parent_path gives the folder");
}

static void Resolve(const std::wstring& setting)
{
    Config::Instance()->NVNGX_DLSS_Library = setting;
    State::Instance().NVNGX_DLSS_Path.reset();
    ResolveOverride();
}

int main(int, char** argv)
{
    const std::filesystem::path root(argv[1]);
    const auto dlss = root / "Engine" / "Plugins" / "DLSS";
    const auto empty = root / "Empty";
    std::filesystem::create_directories(dlss);
    std::filesystem::create_directories(empty);
    std::ofstream(dlss / "nvngx_dlss.dll") << "MZ";

    Resolve(dlss.wstring());
    ExpectFolder(dlss, "a folder without a trailing separator");

    Resolve(dlss.wstring() + L"/");
    ExpectFolder(dlss, "a folder with a trailing separator");

    Resolve((dlss / "nvngx_dlss.dll").wstring());
    ExpectFolder(dlss, "the file itself");

    Resolve(empty.wstring());
    Expect(!State::Instance().NVNGX_DLSS_Path.has_value(), "a folder without the file leaves the search to run");

    Resolve((root / "Missing").wstring());
    Expect(!State::Instance().NVNGX_DLSS_Path.has_value(), "a path that does not exist leaves the search to run");

    if (failures != 0)
        return 1;
    std::puts("dlss-library-path: all cases pass");
    return 0;
}
