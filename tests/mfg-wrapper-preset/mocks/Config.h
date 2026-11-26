#pragma once
#include <string>
template <typename T> struct Option
{
    T value;
    T value_or_default() const { return value; }
    void operator=(T next) { value = next; }
};
struct Config
{
    Option<bool> FGDLSSGAdaMfgUnlock { false };
    Option<std::string> FGDLSSGAdaMfgPreset { "auto" };
    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};
