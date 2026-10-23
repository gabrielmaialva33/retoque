
// ---- cases ----
#include <sys/wait.h>
#include <unistd.h>
#include <functional>
#include <vector>

namespace
{
struct Case
{
    const char* name;
    std::function<void()> body;
};
std::vector<Case>& Cases()
{
    static std::vector<Case> cases;
    return cases;
}
struct Register
{
    Register(const char* name, std::function<void()> body) { Cases().push_back({ name, std::move(body) }); }
};
#define CASE(id, text)                                                                                                 \
    static void id();                                                                                                  \
    static Register reg_##id(text, id);                                                                                \
    static void id()

void Reset()
{
    calls = {};
    window = {};
    State::Instance() = {};
    Config::Instance()->FGXeFGForceBorderless.value = false;
}

// XeFG with ForceBorderless after the game asked for exclusive fullscreen: the real chain is a borderless window.
void EmulatedExclusive()
{
    Reset();
    State::Instance().activeFgOutput = FGOutput::XeFG;
    State::Instance().SCExclusiveFullscreen = true;
    Config::Instance()->FGXeFGForceBorderless.value = true;
}
} // namespace

// ---- GetFullscreenState / GetFullscreenDesc (DXGI: both state arguments are _Out_opt_) ----

CASE(state_flag_only, "GetFullscreenState(&fs, NULL) reports the emulated fullscreen, target argument forwarded NULL")
{
    EmulatedExclusive();
    IDXGISwapChain sc;
    BOOL fs = FALSE;
    assert(FGHooks::hkGetFullscreenState(&sc, &fs, nullptr) == S_OK);
    assert(fs == TRUE);
    assert(calls.getState == 1 && calls.lastGetFullscreenArg == &fs && calls.lastGetTargetArg == nullptr);
    assert(sc.containingCalls == 0);
}

CASE(state_both_null, "GetFullscreenState(NULL, NULL) while emulating writes nothing and forwards both NULLs")
{
    EmulatedExclusive();
    IDXGISwapChain sc;
    assert(FGHooks::hkGetFullscreenState(&sc, nullptr, nullptr) == S_OK);
    assert(calls.getState == 1 && calls.lastGetFullscreenArg == nullptr && calls.lastGetTargetArg == nullptr);
}

CASE(state_target_only, "GetFullscreenState(NULL, &target) while emulating gives the window's output, AddRef'd")
{
    EmulatedExclusive();
    IDXGIOutput monitor;
    IDXGISwapChain sc;
    sc.containing = &monitor;
    IDXGIOutput* target = nullptr;
    assert(FGHooks::hkGetFullscreenState(&sc, nullptr, &target) == S_OK);
    assert(calls.lastGetFullscreenArg == nullptr);
    assert(target == &monitor && monitor.refs == 2);
    target->Release();
    assert(monitor.refs == 1);
}

CASE(state_flag_and_target, "GetFullscreenState(&fs, &target) while emulating: TRUE comes with a target, not NULL")
{
    EmulatedExclusive();
    IDXGIOutput monitor;
    IDXGISwapChain sc;
    sc.containing = &monitor;
    BOOL fs = FALSE;
    IDXGIOutput* target = nullptr;
    assert(FGHooks::hkGetFullscreenState(&sc, &fs, &target) == S_OK);
    assert(fs == TRUE);
    assert(target == &monitor && monitor.refs == 2);
    target->Release();
}

CASE(state_no_containing_output, "No containing output: still TRUE, target left NULL, nothing leaked")
{
    EmulatedExclusive();
    IDXGISwapChain sc; // containing == nullptr: GetContainingOutput fails
    BOOL fs = FALSE;
    IDXGIOutput* target = nullptr;
    assert(FGHooks::hkGetFullscreenState(&sc, &fs, &target) == S_OK);
    assert(fs == TRUE && target == nullptr);
}

CASE(state_real_target_kept, "A target the real chain already returned is kept, not replaced or leaked")
{
    EmulatedExclusive();
    IDXGIOutput real, monitor;
    IDXGISwapChain sc;
    sc.realFullscreen = TRUE;
    sc.realTarget = &real;
    sc.containing = &monitor;
    BOOL fs = FALSE;
    IDXGIOutput* target = nullptr;
    assert(FGHooks::hkGetFullscreenState(&sc, &fs, &target) == S_OK);
    assert(fs == TRUE && target == &real && real.refs == 2 && monitor.refs == 1 && sc.containingCalls == 0);
}

CASE(state_not_emulating, "Not emulating: the real answer is passed through untouched")
{
    for (bool borderless : { false, true })
    {
        for (bool exclusiveFlag : { false, true })
        {
            if (borderless && exclusiveFlag)
                continue; // that is the emulation itself
            Reset();
            State::Instance().SCExclusiveFullscreen = exclusiveFlag;
            Config::Instance()->FGXeFGForceBorderless.value = borderless;
            IDXGIOutput monitor;
            IDXGISwapChain sc;
            sc.containing = &monitor;
            BOOL fs = TRUE;
            IDXGIOutput* target = &monitor;
            assert(FGHooks::hkGetFullscreenState(&sc, &fs, &target) == S_OK);
            assert(fs == FALSE && target == nullptr && monitor.refs == 1 && sc.containingCalls == 0);
        }
    }
}

CASE(desc_reported, "GetFullscreenDesc(&desc) while emulating reports Windowed = FALSE")
{
    EmulatedExclusive();
    IDXGISwapChain1 sc;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC desc;
    assert(FGHooks::hkGetFullscreenDesc(&sc, &desc) == S_OK);
    assert(desc.Windowed == FALSE);
}

CASE(desc_null_refused, "GetFullscreenDesc(NULL): DXGI's refusal is returned as is")
{
    EmulatedExclusive();
    IDXGISwapChain1 sc;
    assert(FGHooks::hkGetFullscreenDesc(&sc, nullptr) == DXGI_ERROR_INVALID_CALL);
    assert(calls.getDesc == 1 && calls.lastGetDescArg == nullptr);
}

CASE(desc_null_lenient_proxy, "GetFullscreenDesc(NULL) answered S_OK by a proxy: nothing is written through NULL")
{
    EmulatedExclusive();
    calls.descNullAnswersOk = true;
    IDXGISwapChain1 sc;
    assert(FGHooks::hkGetFullscreenDesc(&sc, nullptr) == S_OK);
}

// ---- SetFullscreenState / ResizeTarget / the resize's window cover: [XeFG] ForceBorderless is XeFG's ----

namespace
{
constexpr FGOutput kOtherOutputs[] = { FGOutput::FSRFG, FGOutput::DLSSG, FGOutput::Reprojection };

// The key left true in the ini (the menu saves it from the XeFG card) while another output runs.
void LeftoverKey(FGOutput output)
{
    Reset();
    State::Instance().activeFgOutput = output;
    Config::Instance()->FGXeFGForceBorderless.value = true;
}

struct Trace
{
    int setState, resizeTarget, restyles, moves, deactivations;
    BOOL realFullscreen, lastSetFullscreen;
    LONG_PTR style;
    bool scExclusive, realExclusive, fgChanged;
    bool operator==(const Trace&) const = default;
};

// A game's mode changes on one chain: created exclusive, out to a window, back, a mode change, a resize.
Trace Script(FGOutput output, bool key)
{
    Reset();
    State::Instance().activeFgOutput = output;
    Config::Instance()->FGXeFGForceBorderless.value = key;
    IFGFeature fg;
    fg.active = true;
    State::Instance().currentFG = &fg;
    IDXGIOutput monitor;
    IDXGISwapChain sc;
    sc.containing = &monitor;
    sc.realFullscreen = TRUE;
    DXGI_MODE_DESC mode;
    assert(FGHooks::hkSetFullscreenState(&sc, FALSE, nullptr) == S_OK);
    assert(FGHooks::hkSetFullscreenState(&sc, TRUE, &monitor) == S_OK);
    assert(FGHooks::hkResizeTarget(&sc, &mode) == S_OK);
    FGHooks::CoverAfterResizeBuffers(S_OK);
    FGHooks::CoverAfterResizeBuffers1(S_OK);
    assert(FGHooks::hkSetFullscreenState(&sc, FALSE, nullptr) == S_OK);
    const auto& st = State::Instance();
    return { calls.setState,
             calls.resizeTarget,
             window.restyles,
             window.moves,
             fg.deactivations,
             sc.realFullscreen,
             calls.lastSetFullscreen,
             window.style,
             st.SCExclusiveFullscreen,
             st.realExclusiveFullscreen,
             st.fgChanged };
}
} // namespace

CASE(other_leaves_exclusive, "Other outputs, key left on: a chain created exclusive is taken out of it on FALSE")
{
    for (auto output : kOtherOutputs)
    {
        LeftoverKey(output);
        IDXGISwapChain sc;
        sc.realFullscreen = TRUE;
        assert(FGHooks::hkSetFullscreenState(&sc, FALSE, nullptr) == S_OK);
        assert(calls.setState == 1 && calls.lastSetFullscreen == FALSE);
        assert(sc.realFullscreen == FALSE);
        assert(!State::Instance().SCExclusiveFullscreen && !State::Instance().realExclusiveFullscreen);
        assert(window.restyles == 0 && window.moves == 0);
    }
}

CASE(other_enters_exclusive, "Other outputs, key left on: TRUE is forwarded as with the key off, no restyle")
{
    for (auto output : kOtherOutputs)
    {
        LeftoverKey(output);
        IDXGIOutput monitor;
        IDXGISwapChain sc;
        assert(FGHooks::hkSetFullscreenState(&sc, TRUE, &monitor) == S_OK);
        assert(calls.setState == 1 && calls.lastSetFullscreen == TRUE && calls.lastSetTarget == &monitor);
        assert(sc.realFullscreen == TRUE && State::Instance().realExclusiveFullscreen);
        assert(!State::Instance().SCExclusiveFullscreen);
        assert(window.restyles == 0 && window.moves == 0);
    }
}

CASE(other_resize_target, "Other outputs, key left on: ResizeTarget reaches the chain")
{
    for (auto output : kOtherOutputs)
    {
        LeftoverKey(output);
        IDXGISwapChain sc;
        DXGI_MODE_DESC mode;
        assert(FGHooks::hkResizeTarget(&sc, &mode) == S_OK);
        assert(calls.resizeTarget == 1);
    }
}

CASE(bridge_presenter_takeback, "D3D11 bridge, FSR-FG/DLSS-G presenter, key left on: the bridge's take-back works")
{
    // The bridge emulates the game's fullscreen (SCExclusiveFullscreen) and styled the window itself. DXGI's Alt+Enter
    // put the presenter into exclusive fullscreen behind it; the bridge sends it SetFullscreenState(FALSE).
    for (auto output : { FGOutput::FSRFG, FGOutput::DLSSG })
    {
        LeftoverKey(output);
        State::Instance().SCExclusiveFullscreen = true;
        window.style = WS_POPUP | WS_VISIBLE;
        IDXGISwapChain sc;
        sc.realFullscreen = TRUE;
        assert(FGHooks::hkSetFullscreenState(&sc, FALSE, nullptr) == S_OK);
        assert(calls.setState == 1 && sc.realFullscreen == FALSE);
        assert(State::Instance().SCExclusiveFullscreen); // the bridge's, left to the bridge
        assert(window.style == (WS_POPUP | WS_VISIBLE) && window.restyles == 0 && window.moves == 0);
    }
}

CASE(bridge_resize_no_restyle, "D3D11 bridge, key left on: the presenter's resize does not restyle the window")
{
    for (auto output : { FGOutput::FSRFG, FGOutput::DLSSG })
    {
        LeftoverKey(output);
        State::Instance().SCExclusiveFullscreen = true;
        FGHooks::CoverAfterResizeBuffers(S_OK);
        FGHooks::CoverAfterResizeBuffers1(S_OK);
        assert(window.restyles == 0 && window.moves == 0);
    }
}

CASE(other_key_is_inert, "Other outputs: a whole mode-change script traces the same with the key on and off")
{
    for (auto output : kOtherOutputs)
        assert(Script(output, true) == Script(output, false));
}

CASE(xefg_emulates, "XeFG with the key (control): fullscreen emulated, never forwarded, window styled both ways")
{
    Reset();
    State::Instance().activeFgOutput = FGOutput::XeFG;
    Config::Instance()->FGXeFGForceBorderless.value = true;
    IDXGIOutput monitor;
    IDXGISwapChain sc;
    assert(FGHooks::hkSetFullscreenState(&sc, TRUE, &monitor) == S_OK);
    assert(calls.setState == 0 && sc.realFullscreen == FALSE);
    assert(State::Instance().SCExclusiveFullscreen && !State::Instance().realExclusiveFullscreen);
    assert(window.style == (WS_POPUP | WS_VISIBLE) && window.moves == 1);
    DXGI_MODE_DESC mode;
    assert(FGHooks::hkResizeTarget(&sc, &mode) == S_OK && calls.resizeTarget == 0);
    FGHooks::CoverAfterResizeBuffers(S_OK);
    FGHooks::CoverAfterResizeBuffers1(S_OK);
    assert(window.moves == 3);
    assert(FGHooks::hkSetFullscreenState(&sc, FALSE, nullptr) == S_OK);
    assert(calls.setState == 0 && !State::Instance().SCExclusiveFullscreen);
    assert(window.style == (WS_OVERLAPPEDWINDOW | WS_VISIBLE) && window.moves == 4);
    FGHooks::CoverAfterResizeBuffers(S_OK);
    assert(window.moves == 4);
}

CASE(key_off_forwards, "Key off (control): every output forwards SetFullscreenState and ResizeTarget")
{
    for (auto output : { FGOutput::XeFG, FGOutput::FSRFG, FGOutput::DLSSG, FGOutput::Reprojection })
    {
        Reset();
        State::Instance().activeFgOutput = output;
        IDXGISwapChain sc;
        DXGI_MODE_DESC mode;
        assert(FGHooks::hkSetFullscreenState(&sc, TRUE, nullptr) == S_OK && sc.realFullscreen == TRUE);
        assert(FGHooks::hkSetFullscreenState(&sc, FALSE, nullptr) == S_OK && sc.realFullscreen == FALSE);
        assert(FGHooks::hkResizeTarget(&sc, &mode) == S_OK);
        assert(calls.setState == 2 && calls.resizeTarget == 1 && window.restyles == 0);
    }
}

CASE(fg_paused_on_mode_change, "Every output: an active FG is paused by a mode change, key or not")
{
    for (auto output : { FGOutput::XeFG, FGOutput::FSRFG, FGOutput::DLSSG })
    {
        for (bool key : { false, true })
        {
            Reset();
            State::Instance().activeFgOutput = output;
            Config::Instance()->FGXeFGForceBorderless.value = key;
            IFGFeature fg;
            fg.active = true;
            State::Instance().currentFG = &fg;
            IDXGISwapChain sc;
            assert(FGHooks::hkSetFullscreenState(&sc, TRUE, nullptr) == S_OK);
            assert(fg.deactivations == 1 && State::Instance().fgChanged);
        }
    }
}

int main()
{
    int failed = 0;
    for (const auto& c : Cases())
    {
        std::fflush(stdout);
        const pid_t pid = fork();
        if (pid == 0)
        {
            c.body();
            std::fflush(stdout);
            _exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        std::printf("%s  %s\n", ok ? "pass" : "FAIL", c.name);
        failed += ok ? 0 : 1;
    }
    std::printf("FG fullscreen hooks: %zu cases, %d failed\n", Cases().size(), failed);
    return failed == 0 ? 0 : 1;
}
