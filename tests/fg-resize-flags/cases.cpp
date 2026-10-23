
int main()
{
    unsigned cases = 0;
    for (auto hook : { hkResizeBuffers, hkResizeBuffers1 })
    {
        // Creation succeeded with the injected waitable flag. The game's original flags omit it.
        IDXGISwapChain accepted { 64 | 2048 };
        UINT flags = 2;
        hook(&accepted, flags);
        assert(flags == (2 | 64 | 2048));
        ++cases;
        // Retry created a non-waitable chain: even a stale requested bit must not be added now.
        IDXGISwapChain retried { 2048 };
        flags = 2 | 64;
        hook(&retried, flags);
        assert(flags == (2 | 2048));
        ++cases;
        // Alternate chains; the global most-recent flags belong to the other chain.
        State::Instance().SCLastFlags = 2048;
        flags = 0;
        hook(&accepted, flags);
        assert(flags == (64 | 2048));
        State::Instance().SCLastFlags = 64 | 2048;
        flags = 0;
        hook(&retried, flags);
        assert(flags == 2048);
        ++cases;
        accepted.result = -1;
        flags = 2;
        hook(&accepted, flags);
        assert(flags == (2 | 2048));
        ++cases;
        State::Instance().activeFgOutput = FGOutput::NoFG;
        flags = 2;
        hook(&accepted, flags);
        assert(flags == 2);
        State::Instance().activeFgOutput = FGOutput::DLSSG;
        ++cases;
    }
    std::cout << "FG resize flags: " << cases << " cases passed\n";
}
