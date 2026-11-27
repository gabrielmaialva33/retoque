#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <dlssnr/DlssNr_PresentReset.h>

#define LOG_TRACE(...) ((void) 0)
struct Option
{
    bool value = true;
    bool value_or_default() const { return value; }
};
struct Config
{
    Option DlssNrEnabled;
    static Config* Instance()
    {
        static Config cfg;
        return &cfg;
    }
};
namespace sl
{
enum Boolean : char
{
    eFalse,
    eTrue,
    eInvalid
};
struct Constants
{
    Boolean reset = eInvalid;
};
using FrameToken = uint32_t;
using ViewportHandle = uint32_t;
enum Result
{
    eOk,
    eError
};
} // namespace sl
namespace sl1
{
using Boolean = sl::Boolean;
using Constants = sl::Constants;
} // namespace sl1
enum class FGInput
{
    Other,
    NvngxFG,
    DLSSG
};
int fgCalls = 0, forwarded = 0;
struct Input
{
    template <class... Args> void setConstants(Args&&...) { ++fgCalls; }
};
struct State
{
    FGInput activeFgInput = FGInput::Other;
    Input slFGInputs, s_sl1FGInputs;
    static State& Instance()
    {
        static State state;
        return state;
    }
};
bool IsSL1AndFGActive() { return State::Instance().activeFgInput == FGInput::DLSSG; }
std::mutex setConstantsMutex;
sl::Result o_slSetConstants(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&)
{
    ++forwarded;
    return sl::eError;
}
bool o_slSetConstants_interposer_sl1(const sl1::Constants&, uint32_t, uint32_t)
{
    ++forwarded;
    return false;
}
struct StreamlineHooks
{
    static sl::Result hkslSetConstants(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&);
    static bool hkslSetConstants_sl1(const sl1::Constants&, uint32_t, uint32_t);
};

namespace sl
{
enum class RenderAPI
{
    eD3D12
};
}
sl::RenderAPI renderApi = sl::RenderAPI::eD3D12;
namespace DlssNr::SlPresent
{
bool Enabled() { return false; }
void Constants(const sl::Constants&, uint32_t, uint32_t) {}
} // namespace DlssNr::SlPresent
