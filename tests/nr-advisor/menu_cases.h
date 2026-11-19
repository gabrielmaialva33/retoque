int main()
{
    using namespace DlssNr;
    auto& timing = GpuTiming::snapshot;
    timing.valid = true;
    timing.metadata.stage = "after";
    timing.metadata.presentId = 1;
    timing.metadata.evaluationId = 1;
    timing.metadata.contractHash = 1;
    timing.metadata.outputWidth = 1920;
    timing.metadata.outputHeight = 1080;
    ImGui::click = "Measure NR choices###nrAdvisorStart";
    ImGui::drawn = 0;
    RenderAdvisor(&config, true);
    assert(ImGui::drawn == 0);
    config.DlssNrEnabled.value = false;
    RenderAdvisor(&config, false);
    assert(ImGui::drawn == 0);
    config.DlssNrEnabled.value = true;
    config.DlssNrUseProxy.value = true;
    RenderAdvisor(&config, false);
    assert(ImGui::drawn == 0);
    config.DlssNrUseProxy.value = false;
    RenderAdvisor(&config, true);
    assert(!AdvisorRuntime::Active());
    config.timing.Enabled = false;
    RenderAdvisor(&config, false);
    assert(!AdvisorRuntime::Active());
    ImGui::click = "Turn on GPU timing###nrAdvisorTiming";
    RenderAdvisor(&config, false);
    assert(config.timing.Enabled && config.timing.Interval == 1 && configWrites == 2);
    config.timing.Interval = 30;
    ImGui::click = "Use detailed GPU timing###nrAdvisorTimingInterval";
    RenderAdvisor(&config, false);
    assert(config.timing.Interval == 1 && configWrites == 4);
    configWrites = 0;
    ImGui::click = "Measure NR choices###nrAdvisorStart";
    timing.sampleAgeMs = 1001;
    RenderAdvisor(&config, false);
    assert(!AdvisorRuntime::Active());
    timing.sampleAgeMs = 0;
    RenderAdvisor(&config, false);
    assert(AdvisorRuntime::Active() && configWrites == 0);
    ImGui::click.clear();
    RenderAdvisor(&config, false);
    assert(ImGui::text.count("Preparing the image") && ImGui::text.count("Option %u of %u") &&
           ImGui::text.count("Frames prepared: %u"));
    ImGui::click = "Cancel trial###nrAdvisorCancel";
    RenderAdvisor(&config, false);
    assert(!AdvisorRuntime::Active() && configWrites == 0);
    ImGui::click = "Measure NR choices###nrAdvisorStart";
    RenderAdvisor(&config, false);
    AdvisorRuntime::Heartbeat(false, true, true);
    assert(!AdvisorRuntime::Active() && configWrites == 0);
    // Complete a synthetic run: original misses 60 FPS, first reduced-scale candidate meets it.
    auto& session = AdvisorRuntime::session;
    session.Start({ 1, 1 }, 60, 0);
    Advisor::Context context;
    context.device = 1;
    context.width = 1920;
    context.height = 1080;
    session.Observe(context);
    double time = 0;
    uint64_t id = 0;
    while (session.Active())
    {
        const auto token = session.Token();
        time += 1100;
        for (unsigned i = 0; i < 90; ++i)
            session.Accept({ token, ++id, 1, 4, session.Index() == 0 ? 20.0 : 16.0, true }, time);
    }
    session.Restored();
    AdvisorRuntime::Publish();
    assert(session.Recommendation() == 1);
    ImGui::click = "Copy trial report###nrAdvisorCopy";
    RenderAdvisor(&config, false);
    assert(ImGui::tableColumns == 5 && ImGui::text.count("Measurements complete."));
    assert(ImGui::clipboard.find("0.75 / 1") != std::string::npos && configWrites == 0);
    ImGui::click = "Apply measured choice###nrAdvisorApply";
    timing.metadata.contractHash = 2;
    RenderAdvisor(&config, false);
    assert(configWrites == 0);
    timing.metadata.contractHash = 1;
    RenderAdvisor(&config, false);
    assert(configWrites == 1 && config.DlssNrWorkingScale.value == .75f && config.DlssNrCadence.value == 1);
    AdvisorRuntime::Cancel(Advisor::Abort::DeviceLost);
    assert(!AdvisorRuntime::Snapshot().ResultValid());
    session.Start({ 1, 1 }, 60, 0);
    context.afterRR = true;
    session.Observe(context);
    time = 0;
    while (session.Active())
    {
        const auto token = session.Token();
        time += 1100;
        for (unsigned i = 0; i < 90; ++i)
            session.Accept({ token, ++id, 1, 4, session.Index() == 0 ? 20.0 : 16.0, true }, time);
    }
    session.Restored();
    AdvisorRuntime::Publish();
    timing.metadata.stage = "after-rr";
    timing.metadata.workingScale = 1;
    ImGui::click = "Apply measured choice###nrAdvisorApply";
    RenderAdvisor(&config, false);
    assert(configWrites == 2 && config.DlssNrRRWorkingScale.value == .75f && config.DlssNrWorkingScale.value == .75f);
    ImGui::click = "Copy trial report###nrAdvisorCopy";
    RenderAdvisor(&config, false);
    assert(ImGui::clipboard.find("RRWorkingScale") != std::string::npos);
    using Gate = AdvisorPolicy::Gate;
    config.scene = true;
    assert(AdvisorPolicy::CheckConfig(config, false) == Gate::Hidden);
    config.scene = false;
    config.DlssNrStage.value = 1;
    assert(AdvisorPolicy::CheckConfig(config, false) == Gate::Stage);
    config.DlssNrStage.value = 0;
    config.DlssNrCadenceAdaptive.value = true;
    assert(AdvisorPolicy::CheckConfig(config, false) == Gate::Adaptive);
    config.DlssNrCadenceAdaptive.value = false;
    config.DlssNrPeripheryCompression.value = true;
    assert(AdvisorPolicy::CheckConfig(config, false) == Gate::Periphery);
    config.DlssNrPeripheryCompression.value = false;
    config.DlssNrHoldFrame.value = true;
    assert(AdvisorPolicy::CheckConfig(config, false) == Gate::Hold);
    config.DlssNrHoldFrame.value = false;
    assert(AdvisorPolicy::CheckConfig(config, false) == Gate::Ready);
    // The upstream heartbeat calls the real module helper, including when the panel is not drawn.
    assert(AdvisorRuntime::Start({ .75f, 1 }, 60));
    AdvisorMenuTick(&config, false);
    assert(!AdvisorRuntime::Active());
}
