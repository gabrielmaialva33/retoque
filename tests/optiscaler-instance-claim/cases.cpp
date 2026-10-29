static int failures = 0;

static void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

static void Reset(DWORD pid)
{
    g_win = FakeWin {};
    g_win.pid = pid;
}

int main()
{
    // Two proxy names in one folder: the first copy to attach keeps its hooks, the second forwards only
    Reset(100);
    Check(!Attach(L"C:\\game\\winmm.dll", false), "the first copy keeps its hooks");
    Check(Attach(L"C:\\game\\dxgi.dll", false), "a second copy runs in pass-through");
    Check(Attach(L"C:\\game\\version.dll", false), "a third copy runs in pass-through too");
    Check(InstanceClaim::InertCopies() == L"C:\\game\\dxgi.dll;C:\\game\\version.dll",
          "the first copy can read both copies that stood down");
    Check(g_win.debugStrings.size() == 2, "each copy that stood down says so through OutputDebugString");
    Check(g_win.mutexes[L"Local\\OptiScaler.Instance.100"] == 1, "only the first copy's claim stays open");

    // One copy alone: nothing stands down and nothing is reported
    Reset(200);
    Check(!Attach(L"C:\\game\\dxgi.dll", false), "a single copy keeps its hooks");
    Check(InstanceClaim::InertCopies().empty(), "a single copy reports nothing");

    // A child process inherits the environment, not the claim: it has its own pid
    Reset(300);
    Attach(L"C:\\game\\winmm.dll", false);
    Attach(L"C:\\game\\dxgi.dll", false);
    g_win.pid = 301;
    Check(!Attach(L"C:\\game\\dxgi.dll", false), "a child process's first copy keeps its hooks");
    Check(InstanceClaim::InertCopies().empty(), "a child process does not read its parent's note");

    // An excluded process is pass-through already and does not take the claim from the copy that runs
    Reset(400);
    Check(Attach(L"C:\\game\\dxgi.dll", true), "an excluded process is pass-through");
    Check(g_win.mutexes.empty(), "an excluded process makes no claim");
    Check(!Attach(L"C:\\game\\winmm.dll", false), "a real copy after it still keeps its hooks");
    Check(InstanceClaim::InertCopies().empty(), "an excluded process is not reported as a stood-down copy");

    // No claim can be made at all: behave as before this existed
    Reset(500);
    g_win.mutexFails = true;
    Check(!Attach(L"C:\\game\\winmm.dll", false) && !Attach(L"C:\\game\\dxgi.dll", false),
          "without a claim every copy keeps its hooks, as before");

    if (failures != 0)
        return 1;
    std::puts("optiscaler-instance-claim: all cases pass");
    return 0;
}
