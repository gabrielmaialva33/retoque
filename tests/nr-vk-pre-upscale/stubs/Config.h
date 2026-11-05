#pragma once
struct Option
{
    uint32_t value = 0;
    uint32_t value_or_default() const { return value; }
};
struct Config
{
    Option DlssNrEnabled, DlssNrStage;
    static Config* Instance();
};
