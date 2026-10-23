int main()
{
    int cases = 0;
    int failures = 0;
    const auto expect = [&](bool ok, const char* what, const std::string& where)
    {
        if (!ok)
        {
            ++failures;
            std::printf("FAIL  %s  [%s]\n", what, where.c_str());
        }
    };

    for (bool willPresent : { true, false })
        for (bool fgActive : { true, false })
            for (auto output : { FGOutput::DLSSG, FGOutput::FSRFG, FGOutput::XeFG })
                for (bool gameMarkers : { false, true })
                    for (bool useGames : { true, false })
                        for (bool pcl : { true, false })
                            for (bool tokenOk : { true, false })
                            {
                                ++cases;
                                events.clear();
                                world = { gameMarkers, pcl, tokenOk };
                                State::Instance().activeFgOutput = output;
                                Config::Instance()->FGDLSSGUseGamesReflexMarkers.value = useGames;
                                IDXGISwapChain4 sc;
                                sl::FrameToken* token = nullptr;
                                sl::Result tokenResult = sl::Result::eErrorReflexAPI;

                                DlssgPresentStart(&sc, willPresent, fgActive, token, tokenResult);

                                const std::string where = std::string("present ") + (willPresent ? "1" : "0") + " fg " +
                                                          (fgActive ? "1" : "0") + " output " +
                                                          std::to_string((int) output) + " gameMarkers " +
                                                          (gameMarkers ? "1" : "0") + " useGames " +
                                                          (useGames ? "1" : "0") + " pcl " + (pcl ? "1" : "0") +
                                                          " token " + (tokenOk ? "1" : "0");

                                const bool dlssgPresent = willPresent && fgActive && output == FGOutput::DLSSG;
                                const bool ownMarkers = dlssgPresent && (!gameMarkers || !useGames) && pcl;
                                const auto count = [](const char* name)
                                {
                                    int n = 0;
                                    for (const auto& e : events)
                                        n += e == name;
                                    return n;
                                };

                                // DLSS-G refuses to generate a frame unless GetCurrentBackBufferIndex was called
                                // on its swapchain that frame (eFailGetCurrentBackBufferIndexNotCalled), whoever
                                // sends the Reflex markers.
                                expect(count("GetCurrentBackBufferIndex") == (dlssgPresent ? 1 : 0),
                                       "GetCurrentBackBufferIndex exactly once on every DLSS-G present", where);

                                // The marker path is unchanged: OptiScaler's own token and ePresentStart only when
                                // the game's markers are not used and PCL is loaded.
                                expect(count("slGetNewFrameToken") == (ownMarkers ? 1 : 0),
                                       "a frame token only for OptiScaler's own markers", where);
                                expect(count("ePresentStart") == (ownMarkers && tokenOk ? 1 : 0),
                                       "ePresentStart only with OptiScaler's own token", where);
                                expect((tokenResult == sl::Result::eOk) == (ownMarkers && tokenOk),
                                       "tokenResult eOk only with a token", where);
                                expect((token != nullptr) == (ownMarkers && tokenOk), "token set only when made",
                                       where);

                                // The index is queried before a token is made for the present
                                if (ownMarkers)
                                    expect(!events.empty() && events.front() == "GetCurrentBackBufferIndex",
                                           "index queried before slGetNewFrameToken", where);
                            }

    std::printf("FG DLSS-G back buffer index: %d cases, %d failures\n", cases, failures);
    return failures == 0 ? 0 : 1;
}
