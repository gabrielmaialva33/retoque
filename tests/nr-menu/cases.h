int main()
{
    Config c;
    ImGui::Reset();
    ImGui::actions["Minimum brightness"] = .75;
    DlssNr::RenderDarkeningFloor(&c, false);
    assert(c.DlssNrMinRatio.value == .75f && ImGui::actions.empty());
    ImGui::Reset();
    ImGui::actions["Reset##minRatio"] = true;
    DlssNr::RenderDarkeningFloor(&c, false);
    assert(c.DlssNrMinRatio.value == 0 && ImGui::actions.empty());
    c.DlssNrUseProxy = true;
    ImGui::Reset();
    DlssNr::RenderDarkeningFloor(&c, false);
    assert(!ImGui::Saw("Minimum brightness"));
    ImGui::Reset();
    DlssNr::RenderDarkeningFloor(&c, true);
    assert(ImGui::Saw("Minimum brightness"));
    c.DlssNrUseProxy = false;
    auto run = [&](bool vulkan = false)
    {
        DlssNr::RenderPassControls(&c, vulkan);
        assert(ImGui::stack.empty());
        assert(ImGui::actions.empty());
    };
    ImGui::Reset();
    run(true);
    assert(!ImGui::Saw("Requested passes###nrPassCount"));
    assert(c.writes == 0);
    ImGui::Reset();
    c.DlssNrUseProxy.value = true;
    run();
    assert(!ImGui::Saw("Individual pass settings###nrIndividualPassSettings"));
    assert(c.writes == 0);
    ImGui::Reset();
    c.DlssNrUseProxy.value = false;
    run();
    assert(ImGui::Saw("Requested passes###nrPassCount"));
    assert(!ImGui::Saw("Edit pass###nrEditPass"));
    assert(c.writes == 0);
    ImGui::Reset();
    ImGui::actions["Individual pass settings###nrIndividualPassSettings"] = true;
    run();
    assert(c.snapshot.Individual);
    assert(!ImGui::Saw("1/Inherit all master settings###nrClearPass"));
    assert(c.overrides.empty());
    ImGui::Reset();
    ImGui::actions["1/Intensity/Override###override"] = true;
    run();
    assert(c.overrides.at(0).Intensity == 1.0f);
    assert(ImGui::Saw("1/Intensity/Intensity"));
    ImGui::Reset();
    ImGui::actions["1/Intensity/Intensity"] = 0.5;
    run();
    assert(c.overrides.at(0).Intensity == 0.5f);
    ImGui::Reset();
    ImGui::actions["1/Intensity/Override###override"] = false;
    run();
    assert(c.overrides.empty());
    assert(!ImGui::Saw("1/Intensity/Intensity"));
    c.master.Intensity = 1.7f;
    ImGui::Reset();
    ImGui::actions["1/Intensity/Override###override"] = true;
    run();
    assert(c.overrides.at(0).Intensity == 1.7f);
    ImGui::Reset();
    ImGui::actions["1/Inherit all master settings###nrClearPass"] = true;
    run();
    assert(c.overrides.empty());
    ImGui::Reset();
    ImGui::actions["Requested passes###nrPassCount"] = 3;
    ImGui::actions["Edit pass###nrEditPass"] = 3;
    run();
    assert(c.snapshot.Count == 3);
    ImGui::Reset();
    ImGui::actions["3/Local structure/Override###override"] = true;
    ImGui::actions["3/Local tone/Override###override"] = true;
    ImGui::actions["3/Skin structure/Override###override"] = true;
    ImGui::actions["3/Model preset/Override###override"] = true;
    ImGui::actions["3/Model preset/Model preset"] = 2;
    ImGui::actions["3/Style/Override###override"] = true;
    ImGui::actions["3/Style/Style"] = 1;
    ImGui::actions["3/autoMask/Override auto skin mask###nrMaskOverride"] = true;
    ImGui::actions["3/autoMask/Auto skin mask###nrPassMask"] = false;
    run();
    auto third = c.overrides.at(2);
    assert(third.LocalStructure == 1.0f && third.LocalTone == 1.0f && third.SkinStructure == -1.0f &&
           third.Preset == 2 && third.Style == 1 && third.AutoMask == false);
    ImGui::Reset();
    ImGui::actions["Requested passes###nrPassCount"] = 1;
    run();
    assert(!ImGui::Saw("Edit pass###nrEditPass"));
    assert(c.overrides.at(2) == third);
    ImGui::Reset();
    ImGui::actions["Individual pass settings###nrIndividualPassSettings"] = false;
    run();
    assert(c.overrides.at(2) == third);
    assert(!ImGui::Saw("1/Intensity/Override###override"));
    ImGui::Reset();
    ImGui::actions["Requested passes###nrPassCount"] = 3;
    ImGui::actions["Individual pass settings###nrIndividualPassSettings"] = true;
    ImGui::actions["Edit pass###nrEditPass"] = 3;
    run();
    assert(c.overrides.at(2) == third);
    ImGui::Reset();
    ImGui::actions["3/Inherit all master settings###nrClearPass"] = true;
    run();
    assert(c.overrides.empty());
    // One weight set: the per-pass preset override is not drawn at all (DEVELOPMENT.md rule 2).
    DlssNr::ModelLog::configCount = 1;
    ImGui::Reset();
    run();
    assert(!ImGui::Saw("3/Model preset/Override###override"));
    assert(ImGui::Saw("3/Style/Override###override"));
    DlssNr::ModelLog::configCount = -1;
    ImGui::Reset();
    run();
    assert(ImGui::Saw("3/Model preset/Override###override"));
    // Auto mask off: skin is drawn but inert, so a click on its override writes nothing.
    c.master.AutoMask = false;
    ImGui::Reset();
    ImGui::actions["3/Skin structure/Override###override"] = true;
    run();
    assert(ImGui::shownDisabled.count("3/Skin structure/Override###override"));
    assert(c.overrides.empty());
    c.master.AutoMask = true;
    ImGui::Reset();
    ImGui::actions["3/Skin structure/Override###override"] = true;
    run();
    assert(c.overrides.at(2).SkinStructure == -1.0f);
    // Model cadence (docs/dlssnr/design/model-cadence.md, DEVELOPMENT.md rule 2): nothing on native Vulkan; with the
    // driver proxy or without the shader, no control and one line only when a cadence is configured; the
    // frame-generation opt-in, smoothing (cadence-smoothing.md) and the status line only above a cadence of 1; a
    // hand-edited out-of-range value reads as off.
    const std::string combo = "Model cadence###nrCadence", optIn = "Allow with frame generation###nrCadenceFrameGen";
    const std::string smooth = "Smooth the model's frames###nrCadenceSmoothing";
    auto cadence = [&](bool vulkan = false)
    {
        DlssNr::RenderCadenceControls(&c, vulkan);
        assert(ImGui::stack.empty());
        assert(ImGui::actions.empty());
    };
    auto said = [](const char* text)
    {
        return std::any_of(ImGui::texts.begin(), ImGui::texts.end(),
                           [&](const std::string& s) { return s.find(text) != std::string::npos; });
    };
    c.DlssNrCadence.value = 2;
    ImGui::Reset();
    cadence(true);
    assert(!ImGui::Saw(combo) && !ImGui::Saw(optIn) && !ImGui::Saw(smooth) && ImGui::texts.empty());
    c.DlssNrUseProxy.value = true;
    c.DlssNrCadence.value = 1;
    ImGui::Reset();
    cadence();
    assert(!ImGui::Saw(combo) && ImGui::texts.empty());
    c.DlssNrCadence.value = 2;
    ImGui::Reset();
    cadence();
    assert(!ImGui::Saw(combo) && !ImGui::Saw(optIn) && !ImGui::Saw(smooth) && ImGui::texts.size() == 1 &&
           said("driver proxy"));
    c.DlssNrUseProxy.value = false;
    DlssNr::cadenceAvailable = false;
    ImGui::Reset();
    cadence();
    assert(!ImGui::Saw(combo) && ImGui::texts.size() == 1 && said("no model cadence shader"));
    DlssNr::cadenceAvailable = true;
    c.DlssNrCadence.value = 1;
    ImGui::Reset();
    cadence();
    assert(ImGui::Saw(combo) && !ImGui::Saw(optIn) && !ImGui::Saw(smooth));
    ImGui::Reset();
    ImGui::actions[combo] = 1;
    cadence();
    assert(c.DlssNrCadence.value == 2);
    ImGui::Reset();
    cadence();
    assert(ImGui::Saw(combo) && ImGui::Saw(optIn) && ImGui::Saw(smooth) && !c.DlssNrCadenceWithFrameGen.value);
    ImGui::Reset();
    ImGui::actions[optIn] = true;
    cadence();
    assert(c.DlssNrCadenceWithFrameGen.value);
    ImGui::Reset();
    ImGui::actions[smooth] = true;
    cadence();
    assert(c.DlssNrCadenceSmoothing.value);
    ImGui::Reset();
    ImGui::actions[smooth] = false;
    cadence();
    assert(!c.DlssNrCadenceSmoothing.value);
    ImGui::Reset();
    ImGui::actions[combo] = 3;
    cadence();
    assert(c.DlssNrCadence.value == 4);
    ImGui::Reset();
    ImGui::actions[combo] = 0;
    cadence();
    assert(c.DlssNrCadence.value == 1);
    ImGui::Reset();
    cadence();
    assert(ImGui::Saw(combo) && !ImGui::Saw(optIn));
    c.DlssNrCadence.value = 9;
    ImGui::Reset();
    cadence();
    assert(ImGui::Saw(combo) && !ImGui::Saw(optIn) && !ImGui::Saw(smooth));
    // Simple view uses the production gate and the exact helpers used by the full panel.
    Config simple;
    DlssNr::ModelLog::configCount = 2;
    DlssNr::ModelLog::active = { "hnet-crazy-cuckoo-small-fp8", 2 };
    auto view = [&](bool vk = false) { return DlssNr::RenderSimpleView(&simple, vk, 1.0f); };
    ImGui::Reset();
    assert(view());
    assert(ImGui::Saw("Show all settings") && ImGui::Saw("Model preset") && ImGui::Saw("Detail strength"));
    assert(ImGui::Saw("Requested passes###nrPassCount"));
    assert(!ImGui::Saw("Individual pass settings###nrIndividualPassSettings"));
    assert(!ImGui::Saw("Measure NR on the GPU###nrGpuTiming"));
    assert(said("Running:") && DlssNr::GpuTiming::reads == 0);
    assert(simple.writes == 0 && !simple.DlssNrShowAllSettings.value);
    assert(ImGui::sections == std::vector<std::string> { "Sequential model passes" });
    ImGui::Reset();
    ImGui::actions["Model preset"] = 1;
    ImGui::actions["Detail strength"] = 0.65;
    ImGui::actions["Requested passes###nrPassCount"] = 2;
    assert(view());
    assert(ImGui::actions.empty());
    assert(simple.master.Preset == 1 && simple.snapshot.Count == 2 && simple.DlssNrTransferStrength.value == 0.65f);
    Config full;
    ImGui::Reset();
    ImGui::actions["Model preset"] = 1;
    ImGui::actions["Detail strength"] = 0.65;
    ImGui::actions["Requested passes###nrPassCount"] = 2;
    DlssNr::RenderModelPreset(&full);
    DlssNr::RenderDetailStrength(&full);
    DlssNr::RenderPassControls(&full, false);
    assert(ImGui::actions.empty() && full.master.Preset == simple.master.Preset &&
           full.snapshot.Count == simple.snapshot.Count);
    assert(full.DlssNrTransferStrength.value == simple.DlssNrTransferStrength.value && full.writes == simple.writes);
    simple.snapshot.Individual = true;
    simple.overrides[0].Intensity = 0.4f;
    auto saved = simple.overrides;
    ImGui::Reset();
    ImGui::actions["Show all settings"] = true;
    assert(!view());
    assert(simple.DlssNrShowAllSettings.value && ImGui::shown == std::vector<std::string> { "Show all settings" });
    ImGui::Reset();
    assert(!view()); // retained preference, full panel may now run
    ImGui::Reset();
    ImGui::actions["Show all settings"] = false;
    assert(view());
    assert(simple.overrides == saved && simple.snapshot.Individual && simple.master.Preset == 1);
    assert(!ImGui::Saw("Individual pass settings###nrIndividualPassSettings"));
    ImGui::Reset();
    assert(view(true));
    assert(!ImGui::Saw("Minimum brightness"));
    assert(!ImGui::Saw("Requested passes###nrPassCount"));
    simple.DlssNrUseProxy = true;
    ImGui::Reset();
    assert(view());
    assert(!ImGui::Saw("Requested passes###nrPassCount"));
    simple.DlssNrUseProxy = false;
    DlssNr::ModelLog::configCount = 1;
    ImGui::Reset();
    assert(view());
    assert(!ImGui::Saw("Model preset") && said("Running:"));
    DlssNr::ModelLog::configCount = -1;
    ImGui::Reset();
    assert(view());
    assert(ImGui::Saw("Model preset"));
    ImGui::Reset();
    ImGui::actions["Reset##detail"] = true;
    assert(view());
    assert(simple.DlssNrTransferStrength.value == 1.0f);
    simple.timing.Enabled = true;
    ImGui::Reset();
    assert(view());
    assert(DlssNr::GpuTiming::reads == 1 && said("No confirmed GPU sample"));
    assert(!ImGui::Saw("Sample every N eligible evaluations###nrGpuTimingInterval"));
    DlssNr::GpuTiming::snapshot.valid = true;
    ImGui::Reset();
    assert(view());
    assert(ImGui::Saw("This is one historical sample, not the current frame or an average. Use matching contracts in "
                      "the INFO log for comparisons."));
    unsigned reads = DlssNr::GpuTiming::reads;
    ImGui::Reset();
    assert(view(true));
    assert(!ImGui::Saw("Minimum brightness"));
    assert(DlssNr::GpuTiming::reads == reads);
    simple.DlssNrUseProxy = true;
    ImGui::Reset();
    assert(view());
    assert(DlssNr::GpuTiming::reads == reads && simple.timing.Enabled);
    // The look table itself: every value inside its control's range, every look distinct and found as itself.
    for (size_t i = 0; i < DlssNr::Looks::kLooks.size(); ++i)
    {
        const auto& v = DlssNr::Looks::kLooks[i].Value;
        assert(v.Intensity >= 0.0f && v.Intensity <= 2.0f && v.Style <= 2);
        assert(v.LocalStructure >= 0.0f && v.LocalStructure <= 2.0f && v.LocalTone >= 0.0f && v.LocalTone <= 2.0f);
        assert(v.SkinStructure >= -1.0f && v.SkinStructure <= 2.0f);
        assert(v.ColourStrength >= 0.0f && v.ColourStrength <= 4.0f && v.DetailStrength >= 0.0f &&
               v.DetailStrength <= 2.0f);
        assert(DlssNr::Looks::Find(v) == static_cast<int>(i));
        auto nearMiss = v;
        nearMiss.Intensity += 0.01f;
        assert(DlssNr::Looks::Find(nearMiss) != static_cast<int>(i));
    }
    // Looks (docs/dlssnr/design/looks.md): named from the current values; a pick writes the master settings through the
    // locked setter and colour/detail directly; anything else reads Custom.
    simple.DlssNrUseProxy = false;
    ImGui::Reset();
    assert(view());
    assert(ImGui::Saw("Look###nrLook=Default"));
    ImGui::Reset();
    ImGui::actions["Look###nrLook/Subtle"] = 1;
    assert(view());
    assert(simple.master.LocalStructure == 0.7f && simple.master.LocalTone == 0.3f && !simple.master.AutoMask);
    ImGui::Reset();
    assert(view());
    assert(ImGui::Saw("Look###nrLook=Subtle"));
    ImGui::Reset();
    ImGui::actions["Look###nrLook/Gentle"] = 1;
    assert(view());
    assert(simple.master.Intensity == 0.5f && simple.master.AutoMask && simple.DlssNrColourStrength.value == 0.5f &&
           simple.DlssNrTransferStrength.value == 0.85f);
    simple.master.Intensity = 0.42f;
    ImGui::Reset();
    assert(view());
    assert(ImGui::Saw("Look###nrLook=Custom"));
    ImGui::Reset();
    ImGui::actions["Look###nrLook/Cinematic"] = 1;
    assert(view());
    assert(simple.master.Style == 2 && simple.master.Intensity == 1.0f && simple.DlssNrColourStrength.value == 1.0f);
    ImGui::Reset();
    ImGui::actions["Look###nrLook/Default"] = 1;
    assert(view());
    {
        auto expected = DlssNrResolvedPassSettings {};
        expected.Preset = simple.master.Preset; // looks never touch the weight set
        assert(simple.master == expected && simple.DlssNrTransferStrength.value == 1.0f);
    }
    std::cout << "PASS: simple/full visibility, same Config writes, retained hidden settings, preset and backend "
                 "gates, read-only timing and balanced item widths\n";
    std::cout << "PASS: 33 scripted menu frames: unsupported paths, inheritance, seven fields, clear, retained "
                 "inactive passes, preset hidden at one weight set, skin inert with the mask off; model cadence hidden "
                 "on native Vulkan, a line only under the proxy or without its shader, the frame-generation opt-in and "
                 "smoothing only above a cadence of 1\n";
}
