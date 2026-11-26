#include <cassert>
#include <cstdint>
#include <cstdio>
#define LOG_WARN(...) ((void) 0)
namespace sl
{
using FrameToken = uint32_t;
enum class PCLMarker
{
    ePresentStart,
    ePresentEnd,
    eSimulationStart,
    eSimulationEnd,
    eRenderSubmitStart
};
enum class Result
{
    eOk,
    eErrorFeatureMissing,
    eError
};
enum class RenderAPI
{
    eD3D12,
    eVulkan
};
} // namespace sl
enum class FGInput
{
    Other,
    DLSSG
};
namespace GameQuirk
{
constexpr unsigned FixSlSimulationMarkers = 1;
}
struct Inputs
{
    unsigned marks = 0, evaluates = 0;
    void markPresent(uint32_t) { ++marks; }
    void evaluateState() { ++evaluates; }
};
struct State
{
    unsigned gameQuirks = 0;
    FGInput activeFgInput = FGInput::Other;
    struct
    {
        int major = 2;
    } streamlineVersion;
    Inputs slFGInputs, s_sl1FGInputs;
    static State& Instance()
    {
        static State s;
        return s;
    }
};
struct InputCollection
{
    static InputCollection& getInstance()
    {
        static InputCollection c;
        return c;
    }
    void startCollectingForFrame(uint32_t) {}
};
sl::RenderAPI renderApi = sl::RenderAPI::eD3D12;
unsigned forwarded = 0, starts = 0, ends = 0;
uint32_t startedToken = 0;
bool enabled = false;
sl::Result originalResult = sl::Result::eOk;
sl::Result Original(sl::PCLMarker, const sl::FrameToken&)
{
    ++forwarded;
    return originalResult;
}
sl::Result (*o_slPCLSetMarker)(sl::PCLMarker, const sl::FrameToken&) = Original;
sl::Result (*o_slGetNewFrameToken)(sl::FrameToken*&, const uint32_t*) = nullptr;
namespace DlssNr::SlPresent
{
bool Enabled() { return enabled; }
void MarkerStart(uint32_t token)
{
    ++starts;
    startedToken = token;
}
void MarkerEnd() { ++ends; }
} // namespace DlssNr::SlPresent
struct StreamlineHooks
{
    static sl::Result hkslPCLSetMarker(sl::PCLMarker, const sl::FrameToken&);
};
