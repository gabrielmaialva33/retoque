#pragma once
// The keys DlssNr_ExposureScan.cpp reads, with a value the cases set.
#include <string>

template <class T> struct TestOption
{
    T value {};
    T value_or_default() const { return value; }
    TestOption& operator=(const T& v)
    {
        value = v;
        return *this;
    }
};

class Config
{
  public:
    static Config* Instance()
    {
        static Config config;
        return &config;
    }

    TestOption<bool> DlssNrEnabled { true };
    TestOption<int> DlssNrWhitePointSource { 2 };
    TestOption<bool> DlssNrScanExposure { false };
    TestOption<bool> DlssNrScanMatch { false };
    TestOption<std::string> DlssNrScanAnchors {};
    TestOption<float> DlssNrScanAnchorValue { 0.0f };
    TestOption<float> DlssNrScanAnchorWhitePoint { 0.0f };
};
