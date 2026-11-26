#include <algorithm>
#include <cassert>
#include <cctype>
#include <concepts>
#include <format>
#include <optional>
#include <string>
#include <vector>
#include <cstdio>
#define SI_NO_CONVERSION
#include <SimpleIni.h>
#include "DlssgPresetPolicy.h"
// @@OPTIONAL@@
class Config
{
  public:
    // @@DECLARATION@@
    CSimpleIniA ini;
    std::vector<std::string> _log;
    static Config* current;
    static Config* Instance() { return current; }
    std::optional<std::string> readString(std::string section, std::string key, bool lowercase = false);
    void Read()
    { // @@READ@@
    }
    void Save()
    {
        current = this; // @@SAVE@@
    }
};
Config* Config::current = nullptr;
// @@READSTRING@@
int main()
{
    for (const char* text : { "auto", "AUTO", "a", "B", "c", "junk", "4" })
    {
        Config config;
        config.ini.SetValue("DLSSG", "AdaMfgPreset", text);
        config.Read();
        const auto parsed = DlssgPresetPolicy::Parse(text).value_or(DlssgPresetPolicy::Preset::Auto);
        assert(config.FGDLSSGAdaMfgPreset.value_or_default() == DlssgPresetPolicy::Name(parsed));
        config.Save();
        assert(std::string(config.ini.GetValue("DLSSG", "AdaMfgPreset")) == DlssgPresetPolicy::Name(parsed));
        std::string saved;
        config.ini.Save(saved);
        Config restored;
        restored.ini.LoadData(saved);
        restored.Read();
        assert(restored.FGDLSSGAdaMfgPreset.value_or_default() == config.FGDLSSGAdaMfgPreset.value_or_default());
    }
    Config missing;
    missing.Read();
    missing.Save();
    assert(std::string(missing.ini.GetValue("DLSSG", "AdaMfgPreset")) == "auto");
    missing.FGDLSSGAdaMfgPreset = "c";
    missing.Save();
    assert(std::string(missing.ini.GetValue("DLSSG", "AdaMfgPreset")) == "c");
    missing.FGDLSSGAdaMfgPreset.reset();
    missing.Save();
    assert(std::string(missing.ini.GetValue("DLSSG", "AdaMfgPreset")) == "auto");
    std::puts("Production Config read/save/read: PASS");
}
