static int failures = 0;

static void Expect(bool ok, const char* what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

int main()
{
    // A Streamline proxy over a real factory: every production site leaves the base's count as it found it
    for (int site = 0; site < 3; ++site)
    {
        IDXGIFactory real;
        IDXGIFactory proxy;
        proxy.base = &real;
        real.refs = 2; // the proxy's own reference plus the game's

        for (int i = 0; i < 8; ++i) // one per DLSS-G swapchain the game creates
        {
            if (site == 0)
                SiteCreateSwapchain(&proxy);
            else if (site == 1)
                SiteCreateSwapchain1(&proxy);
            else
                SiteHookToDLSSGFactory(&proxy);
        }

        Expect(real.refs == 2, site == 0   ? "CreateSwapchainInternal leaves the real factory's count unchanged"
                               : site == 1 ? "CreateSwapchain1Internal leaves the real factory's count unchanged"
                                           : "HookToDLSSGFactory leaves the real factory's count unchanged");
        Expect(proxy.refs == 1, "the proxy's own count is untouched");
    }
    Expect(StreamlineProxy::upgrades == 0, "a proxy is not upgraded again");

    // A plain factory: upgraded and hooked, nothing released
    {
        IDXGIFactory plain;
        SiteCreateSwapchain(&plain);
        SiteCreateSwapchain1(&plain);
        Expect(plain.refs == 1, "a plain factory's count is untouched");
        Expect(StreamlineProxy::upgrades == 2 && hookCalls == 2, "a plain factory is upgraded and hooked");
    }

    // The hook itself on a plain factory: nothing to hook, nothing released
    {
        IDXGIFactory plain;
        hooked = false;
        SiteHookToDLSSGFactory(&plain);
        Expect(!hooked && plain.refs == 1, "HookToDLSSGFactory leaves a plain factory alone");
    }

    if (failures != 0)
        return 1;
    std::puts("fg-streamline-real-object: all cases pass");
    return 0;
}
