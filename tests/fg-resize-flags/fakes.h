#include <cassert>
#include <iostream>
using UINT = unsigned int;
constexpr int S_OK = 0;
constexpr UINT DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH = 2;
constexpr UINT DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT = 64;
constexpr UINT DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING = 2048;
struct DXGI_SWAP_CHAIN_DESC
{
    UINT Flags = 0;
};
struct IDXGISwapChain
{
    UINT flags;
    int result = S_OK;
    int GetDesc(DXGI_SWAP_CHAIN_DESC* desc)
    {
        desc->Flags = flags;
        return result;
    }
};
enum class FGOutput
{
    DLSSG,
    XeFG,
    NoFG
};
struct State
{
    FGOutput activeFgOutput = FGOutput::DLSSG;
    UINT SCLastFlags = 0;
    static State& Instance()
    {
        static State s;
        return s;
    }
};
struct Option
{
    bool value_or_default() { return false; }
};
struct Config
{
    Option FGXeFGForceBorderless;
    static Config* Instance()
    {
        static Config c;
        return &c;
    }
};
#define LOG_DEBUG(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)
