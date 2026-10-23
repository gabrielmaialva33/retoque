// Fakes for FGPresent's DLSS-G block, sliced verbatim from OptiScaler/hooks/FG_Hooks.cpp.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using UINT = unsigned int;

inline std::vector<std::string> events;

namespace sl
{
enum class Result
{
    eOk,
    eErrorReflexAPI,
    eErrorMissingInputParameter
};
enum class PCLMarker
{
    ePresentStart,
    ePresentEnd
};
struct FrameToken
{
    uint32_t id = 0;
};
} // namespace sl

struct IDXGISwapChain
{
    virtual ~IDXGISwapChain() = default;
};
struct IDXGISwapChain4 : IDXGISwapChain
{
    UINT index = 1;
    UINT GetCurrentBackBufferIndex()
    {
        events.push_back("GetCurrentBackBufferIndex");
        return index;
    }
};

struct Feature
{
    uint64_t FrameCount() { return 42; }
};

enum class FGOutput
{
    NoFG,
    FSRFG,
    DLSSG,
    XeFG,
    Reprojection
};
struct State
{
    FGOutput activeFgOutput = FGOutput::DLSSG;
    static State& Instance()
    {
        static State s;
        return s;
    }
};
struct Option
{
    bool value = true;
    bool value_or_default() { return value; }
};
struct Config
{
    Option FGDLSSGUseGamesReflexMarkers;
    static Config* Instance()
    {
        static Config c;
        return &c;
    }
};

struct World
{
    bool gameMarkers = false;
    bool pclLoaded = true;
    bool tokenOk = true;
};
inline World world;

namespace ReflexHooks
{
inline bool gameIsSendingMarkers() { return world.gameMarkers; }
} // namespace ReflexHooks

struct ScopedOptiScalerReflex
{
};

inline sl::FrameToken fakeToken;
inline sl::Result FakeGetNewFrameToken(sl::FrameToken*& token, const uint32_t* frameIndex)
{
    events.push_back("slGetNewFrameToken");
    if (!world.tokenOk)
        return sl::Result::eErrorMissingInputParameter;
    fakeToken.id = frameIndex != nullptr ? *frameIndex : 0;
    token = &fakeToken;
    return sl::Result::eOk;
}
inline sl::Result FakePCLSetMarker(sl::PCLMarker marker, const sl::FrameToken&)
{
    events.push_back(marker == sl::PCLMarker::ePresentStart ? "ePresentStart" : "ePresentEnd");
    return sl::Result::eOk;
}

struct StreamlineProxy
{
    using PFN_slGetNewFrameToken = sl::Result (*)(sl::FrameToken*&, const uint32_t*);
    using PFN_slPCLSetMarker = sl::Result (*)(sl::PCLMarker, const sl::FrameToken&);
    static PFN_slGetNewFrameToken GetNewFrameToken() { return FakeGetNewFrameToken; }
    static PFN_slPCLSetMarker PCLSetMarker() { return world.pclLoaded ? FakePCLSetMarker : nullptr; }
};

inline Feature feature;
