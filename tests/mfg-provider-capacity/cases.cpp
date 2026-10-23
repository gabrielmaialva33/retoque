
void require(bool yes, const char* why)
{
    if (!yes)
        throw std::runtime_error(why);
}
void roundTrip()
{
    auto* cfg = Config::Instance();
    cfg->Save();
    std::string saved;
    require(cfg->ini.Save(saved) >= 0, "INI save failed");
    Config reloaded;
    require(reloaded.ini.LoadData(saved) >= 0, "INI load failed");
    reloaded.Read();
    require(reloaded.FGDLSSGOverrideInterpolationCount.value() == 3, "saved 4x preference was lost");
}
int main(int argc, char** argv)
{
    assert(argc == 2);
    const std::string name = argv[1];
    auto* cfg = Config::Instance();
    cfg->ini.SetValue("DLSSG", "OverrideInterpolationCount", "3");
    cfg->Read();
    auto& state = State::Instance();
    const sl::ViewportHandle viewport { 0 };
    sl::DLSSGOptions options {};
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = 1;
    auto set = [&] { return StreamlineHooks::hkslDLSSGSetOptions(viewport, options); };
    auto get = [&]
    {
        sl::DLSSGState response {};
        return StreamlineHooks::hkslDLSSGGetState(viewport, response, &options);
    };
    if (name == "setter-preference" || name == "getter-preference")
    {
        if (name == "setter-preference")
        {
            require(set() == sl::Result::eOk, "setter failed");
            require(Provider::sent.numFramesToGenerate == 1, "limited provider received unsupported count");
        }
        else
            require(get() == sl::Result::eOk, "getter failed");
        roundTrip();
    }
    else if (name == "setter-refresh" || name == "getter-refresh")
    {
        if (name == "setter-refresh")
            set();
        else
            get();
        require(state.dlssgMfgMax == 1, "first provider capacity missing");
        // The original 4x preference must recover without another user/config write.
        Provider::max = 3;
        ++MfgUnlock::generation; // Load/patch event, including patching at the same entry point.
        if (name == "getter-refresh")
            StreamlineHooks::RebindGetter(Provider::NewGet);
        if (name == "setter-refresh")
            set();
        else
            get();
        require(state.dlssgMfgMax == 3, "new/patched provider capacity stayed latched at 1");
        set();
        require(Provider::sent.numFramesToGenerate == 3, "4x preference did not recover");
        roundTrip();
    }
    else if (name == "downgrade")
    {
        Provider::max = 3;
        set();
        Provider::max = 1;
        ++MfgUnlock::generation;
        set();
        require(state.dlssgMfgMax == 1 && Provider::sent.numFramesToGenerate == 1, "provider downgrade ignored");
        roundTrip();
    }
    else if (name == "query-failure")
    {
        Provider::max = 3;
        get();
        Provider::max = 1;
        Provider::result = sl::Result::eErrorNotInitialized;
        require(get() != sl::Result::eOk, "failed query claimed success");
        require(state.dlssgMfgMax == 3, "failed query replaced last valid capacity");
        Provider::result = sl::Result::eOk;
        Provider::max = 0;
        get();
        require(state.dlssgMfgMax == 3, "zero capacity accepted");
        Provider::max = 6;
        get();
        require(state.dlssgMfgMax == 3, "out-of-range capacity accepted");
        roundTrip();
    }
    else if (name == "old-structs")
    {
        for (unsigned version : { 1u, 2u, 3u, 4u })
        {
            Provider::max = version < 3 ? 1 : 3;
            sl::DLSSGState response {};
            response.structVersion = version;
            StreamlineHooks::hkslDLSSGGetState(viewport, response, &options);
            require(response.structVersion == version, "caller struct version overwritten");
            require(state.dlssgMfgMax == static_cast<int>(Provider::max), "old caller missed new capacity");
        }
        roundTrip();
    }
    else if (name == "menu-transitions")
    {
        set(); // A limited provider must not replace the preference the hotkey remembers.
        auto& ratio = cfg->FGDLSSGOverrideInterpolationCount;
        const int saved = ratio.has_value() ? ratio.value() : -1;
        ratio = 0;
        set();
        require(Provider::sent.mode == sl::DLSSGMode::eOff, "hotkey off ignored");
        ratio = saved;
        set();
        roundTrip();
        ratio.reset(); // The menu's Default action, after a capacity clamp.
        get();
        set();
        require(!ratio.has_value() && !ratio.value_for_config().has_value(), "Default revived the old override");
        require(Provider::sent.numFramesToGenerate == 1, "Default did not use the game's count");
    }
    else if (name == "stable-queries")
    {
        for (unsigned i = 0; i < 64; ++i)
            set();
        require(Provider::gets == 1, "stable SetOptions queried the provider more than once");
        require(MfgUnlock::tries == 1, "stable SetOptions retried the patcher");
        Provider::max = 3;
        ++MfgUnlock::generation;
        for (unsigned i = 0; i < 64; ++i)
            set();
        require(Provider::gets == 2 && MfgUnlock::tries == 2, "new generation did not query exactly once");
        require(Provider::sent.numFramesToGenerate == 3, "new generation did not recover 4x");
        roundTrip();
    }
    else if (name == "setter-rebind")
    {
        set();
        Provider::max = 3;
        StreamlineHooks::RebindGetter(Provider::NewGet); // Wrapper-only rebind, no snippet generation event.
        for (unsigned i = 0; i < 64; ++i)
            set();
        require(Provider::gets == 2 && MfgUnlock::tries == 2, "getter rebind did not query exactly once");
        require(Provider::sent.numFramesToGenerate == 3, "getter rebind kept the previous wrapper's cap");
        roundTrip();
    }
    else if (name == "game-get-free")
    {
        for (unsigned i = 0; i < 16; ++i)
            get();
        require(Provider::gets == 16 && MfgUnlock::tries == 0, "game GetState invoked the patcher or extra queries");
        for (unsigned i = 0; i < 16; ++i)
            set();
        require(Provider::gets == 16 && MfgUnlock::tries == 0, "setter ignored the game's cached response");
        ++MfgUnlock::generation;
        Provider::max = 3;
        get();
        set();
        require(Provider::gets == 17 && MfgUnlock::tries == 0, "new game response did not refresh the generation");
        require(Provider::sent.numFramesToGenerate == 3, "game response did not recover 4x");
    }
    else if (name == "patch-during-probe")
    {
        MfgUnlock::patchOnTry = true;
        set();
        set();
        require(Provider::gets == 1 && MfgUnlock::tries == 1, "patcher's own generation caused a second query");
        ++MfgUnlock::generation;
        Provider::duringGet = [] { ++MfgUnlock::generation; };
        set();
        set();
        set();
        require(Provider::gets == 3 && MfgUnlock::tries == 3, "generation arriving during a query was lost");
    }
    else if (name == "auto-off")
    {
        cfg->FGDLSSGOverrideInterpolationCount.reset();
        set();
        require(!cfg->FGDLSSGOverrideInterpolationCount.has_value(), "auto preference materialized");
        cfg->FGDLSSGOverrideInterpolationCount = 0;
        set();
        require(Provider::sent.mode == sl::DLSSGMode::eOff, "explicit off overridden");
    }
    else
        throw std::runtime_error("unknown case");
    std::cout << "PASS: " << name << "\n";
}
