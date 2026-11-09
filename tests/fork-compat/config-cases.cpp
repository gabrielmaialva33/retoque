
void expect(Config& config, const std::optional<std::wstring>& sr, const std::optional<std::wstring>& fg,
            const std::optional<std::wstring>& ll)
{
    assert(config.XeSSLibrary.has_value() == sr.has_value());
    if (sr)
        assert(config.XeSSLibrary.value() == sr.value());
    assert(config.XeFGLibrary.has_value() == fg.has_value());
    if (fg)
        assert(config.XeFGLibrary.value() == fg.value());
    assert(config.XeLLLibrary.has_value() == ll.has_value());
    if (ll)
        assert(config.XeLLLibrary.value() == ll.value());
}

int main()
{
    struct Case
    {
        const char* sr;
        const char* fg;
        const char* ll;
        std::optional<std::wstring> wantSr;
        std::optional<std::wstring> wantFg;
        std::optional<std::wstring> wantLl;
    };
    const Case cases[] = {
        { "C:\\SR\\libxess.dll", "D:\\FG\\libxess_fg.dll", "E:\\LL\\libxell.dll", L"C:\\SR\\libxess.dll",
          L"D:\\FG\\libxess_fg.dll", L"E:\\LL\\libxell.dll" },
        { "auto", "D:\\FG\\libxess_fg.dll", "auto", std::nullopt, L"D:\\FG\\libxess_fg.dll", std::nullopt },
        { "auto", "auto", "E:\\LL\\libxell.dll", std::nullopt, std::nullopt, L"E:\\LL\\libxell.dll" },
        { "C:\\SR\\libxess.dll", "auto", "auto", L"C:\\SR\\libxess.dll", std::nullopt, std::nullopt },
        { "auto", "AUTO", "Auto", std::nullopt, std::nullopt, std::nullopt },
        { nullptr, nullptr, nullptr, std::nullopt, std::nullopt, std::nullopt },
    };
    for (const auto& test : cases)
    {
        Config first;
        if (test.sr)
            first.ini.SetValue("Libraries", "XeSSPath", test.sr);
        if (test.fg)
            first.ini.SetValue("Libraries", "XeFGPath", test.fg);
        if (test.ll)
            first.ini.SetValue("Libraries", "XeLLPath", test.ll);
        first.Read();
        expect(first, test.wantSr, test.wantFg, test.wantLl);
        first.ini.Reset();
        first.Save();
        std::string saved;
        assert(first.ini.Save(saved) >= 0);
        Config second;
        assert(second.ini.LoadData(saved) >= 0);
        second.Read();
        expect(second, test.wantSr, test.wantFg, test.wantLl);
        for (const auto* key : { "XeSSPath", "XeFGPath", "XeLLPath" })
            assert(std::string(second.ini.GetValue("Libraries", key)) != "AUTO");
    }
    for (const char* value : { "auto", "true", "false", "TRUE", "invalid" })
    {
        Config first;
        first.ini.SetValue("OptiFG", "HudlessFlip", value);
        first.ini.SetValue("Log", "CreateDumps", value);
        first.Read();
        const bool expected = std::string(value) == "true" || std::string(value) == "TRUE";
        assert(first.FGHudlessFlip.value_or_default() == expected);
        assert(first.CreateDumps.value_or_default() == expected);
        first.ini.Reset();
        first.Save();
        std::string saved;
        assert(first.ini.Save(saved) >= 0);
        Config second;
        assert(second.ini.LoadData(saved) >= 0);
        second.Read();
        assert(second.FGHudlessFlip.value_or_default() == expected);
        assert(second.CreateDumps.value_or_default() == expected);
    }
    for (const char* mode : { "auto", "0", "1", "2", "3", "1000361000", "invalid" })
    {
        Config first;
        first.ini.SetValue("V-Sync", "VulkanPresentMode", mode);
        first.Read();
        first.ini.Reset();
        first.Save();
        std::string saved;
        assert(first.ini.Save(saved) >= 0);
        Config second;
        assert(second.ini.LoadData(saved) >= 0);
        second.Read();
        assert(first.VulkanPresentMode.value_for_config() == second.VulkanPresentMode.value_for_config());
    }
    // Runtime resolution or a missing DLL must not replace the persisted relative preference.
    CustomOptional<std::wstring, NoDefault> path;
    path.set_from_config(L"relative/model.dll");
    path.set_volatile_value(L"Z:/resolved/relative/model.dll");
    assert(path.value_for_config() == L"relative/model.dll");
    path.reset_volatile();
    assert(!path.has_value() && path.value_for_config() == L"relative/model.dll");
    path = std::wstring(L"new.dll");
    assert(path.value_for_config() == L"new.dll");
    CustomOptional<std::wstring, NoDefault> automatic;
    automatic.set_volatile_value(L"Z:/found/model.dll");
    automatic.reset_volatile();
    assert(!automatic.has_value() && !automatic.value_for_config().has_value());
    std::cout << "PASS: new INI keys read/save/read, auto/defaults, volatile paths and reset\n";
    std::cout << "PASS: distinct XeSS/XeFG/XeLL paths, single overrides, auto/missing and read/save/read\n";
}
