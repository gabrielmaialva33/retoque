
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
