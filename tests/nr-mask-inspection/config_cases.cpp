// [DlssNr] MaskInspection through the production CustomOptional, Config::readString and the key's own read and save
// statements (spliced in by run.py), against the real SimpleIni. No Config.cpp is linked.
#include <algorithm>
#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

#define SI_NO_CONVERSION
#include <SimpleIni.h>

#include "retoque/dlssnr/DlssNr_MaskInspection.h"

#define REQUIRE(condition)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (false)

static CSimpleIniA ini;

// @@CUSTOM_OPTIONAL@@

static std::string GetBoolValue(std::optional<bool> value)
{
    return value.has_value() ? (*value ? "true" : "false") : "auto";
}

class Config
{
  public:
    CustomOptional<bool> DlssNrEnabled { false };
    CustomOptional<bool> DlssNrUiProtection { false };
    // @@DECLARATION@@
    std::vector<std::string> _log;

    static Config*& Current()
    {
        static Config* current = nullptr;
        return current;
    }
    static Config* Instance() { return Current(); }

    std::optional<std::string> readString(std::string section, std::string key, bool lowercase = false);
    std::optional<bool> readBool(std::string section, std::string key)
    {
        const auto value = readString(section, key, true);
        if (!value)
            return std::nullopt;
        return *value == "true";
    }

    void Load()
    {
        DlssNrEnabled.set_from_config(readBool("DlssNr", "Enabled"));
        DlssNrUiProtection.set_from_config(readBool("DlssNr", "UiProtection"));
        // @@READ@@
    }

    void Save()
    {
        ini.SetValue("DlssNr", "Enabled", GetBoolValue(Instance()->DlssNrEnabled.value_for_config()).c_str());
        ini.SetValue("DlssNr", "UiProtection", GetBoolValue(Instance()->DlssNrUiProtection.value_for_config()).c_str());
        // @@SAVE@@
    }
};

// @@READ_STRING@@

namespace
{
using DlssNr::MaskInspection::Requested;

struct Session
{
    Config config;

    explicit Session(const std::string& text)
    {
        ini.Reset();
        REQUIRE(ini.LoadData(text.c_str()) >= 0);
        Config::Current() = &config;
        config.Load();
    }

    // What a save writes, and the whole file it leaves.
    std::string Saved(std::string* file = nullptr)
    {
        config.Save();
        std::string text;
        REQUIRE(ini.Save(text) >= 0);
        if (file != nullptr)
            *file = text;
        const char* value = ini.GetValue("DlssNr", "MaskInspection");
        REQUIRE(value != nullptr);
        return value;
    }
};

std::string Ini(const char* value)
{
    return std::string("[DlssNr]\nEnabled=true\nUiProtection=true\n") +
           (value != nullptr ? std::string("MaskInspection=") + value + "\n" : std::string()) +
           "[Unrelated]\nKeep=original\n";
}

size_t Logged(const Config& config)
{
    return static_cast<size_t>(std::ranges::count_if(config._log, [](const std::string& entry)
                                                     { return entry.starts_with("DlssNr.MaskInspection: "); }));
}

void UnsetAutoAndOffAreOff()
{
    for (const char* value : { static_cast<const char*>(nullptr), "auto", "AUTO" })
    {
        Session s(Ini(value));
        REQUIRE(!s.config.DlssNrMaskInspection.has_value());
        REQUIRE(s.config.DlssNrMaskInspection.value_or_default() == "off");
        REQUIRE(s.config.DlssNrEnabled.value_or_default() && !Requested(s.config));
        REQUIRE(Logged(s.config) == 0);
        REQUIRE(s.Saved() == "auto"); // what it resolved to is not written as a choice
    }

    Session off(Ini("off"));
    REQUIRE(off.config.DlssNrMaskInspection.has_value() && !Requested(off.config));
    REQUIRE(Logged(off.config) == 1);
    REQUIRE(off.Saved() == "auto"); // the default, as every key saves its default
}

void LogRoundTrips()
{
    for (const char* value : { "log", "LOG", "Log" })
    {
        Session s(Ini(value));
        REQUIRE(Requested(s.config));
        REQUIRE(Logged(s.config) == 1);

        std::string file;
        REQUIRE(s.Saved(&file) == "log");
        for (const char* kept : { "Enabled = true", "UiProtection = true", "[Unrelated]", "Keep = original" })
            REQUIRE(file.find(kept) != std::string::npos);

        Session reloaded(file);
        REQUIRE(Requested(reloaded.config));
        REQUIRE(reloaded.config.DlssNrUiProtection.value_or_default());
        REQUIRE(reloaded.Saved() == "log");
    }
}

void TheRequestIsNotTheEffectiveState()
{
    // NR disabled: nothing is observed, and the request is kept as asked.
    Session disabled("[DlssNr]\nEnabled=false\nMaskInspection=log\n");
    REQUIRE(!Requested(disabled.config));
    REQUIRE(disabled.Saved() == "log");
    Session back("[DlssNr]\nEnabled=true\nMaskInspection=log\n");
    REQUIRE(Requested(back.config));

    // A value set for the session (a game profile, a test) is never saved over the user's.
    Session session(Ini(nullptr));
    session.config.DlssNrMaskInspection.set_volatile_value("log");
    REQUIRE(Requested(session.config));
    REQUIRE(session.Saved() == "auto");
    Session asked(Ini("log"));
    asked.config.DlssNrMaskInspection.set_volatile_value("off");
    REQUIRE(!Requested(asked.config));
    REQUIRE(asked.Saved() == "log");
}

void MalformedIsOffAndKeptAsWritten()
{
    for (const char* value : { "overlay", "true", "1", "on", "logs", "log,overlay", "lo g", "off|log", "" })
    {
        Session s(Ini(value));
        REQUIRE(!Requested(s.config));
        REQUIRE(Logged(s.config) == 1); // the existing config diagnostic, once; no warning of its own
        std::string file;
        REQUIRE(s.Saved(&file) == value);
        REQUIRE(file.find("Keep = original") != std::string::npos);
        Session reloaded(file);
        REQUIRE(!Requested(reloaded.config));
    }
}

void TheShippedIniIsOff(const char* path)
{
    ini.Reset();
    REQUIRE(ini.LoadFile(path) >= 0);
    const char* shipped = ini.GetValue("DlssNr", "MaskInspection");
    REQUIRE(shipped != nullptr && std::string(shipped) == "auto");
    Config config;
    Config::Current() = &config;
    config.Load();
    REQUIRE(!config.DlssNrMaskInspection.has_value() && !Requested(config));

    // Even with NR switched on over the shipped file.
    config.DlssNrEnabled = true;
    REQUIRE(!Requested(config));
}
} // namespace

int main(int argc, char** argv)
{
    REQUIRE(argc == 2);
    UnsetAutoAndOffAreOff();
    LogRoundTrips();
    TheRequestIsNotTheEffectiveState();
    MalformedIsOffAndKeptAsWritten();
    TheShippedIniIsOff(argv[1]);
    std::puts("PASS: [DlssNr] MaskInspection config cases");
    return 0;
}
