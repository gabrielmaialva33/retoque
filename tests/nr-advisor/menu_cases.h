int main()
{
    using namespace DlssNr;
    auto& timing = GpuTiming::snapshot;
    timing.valid = true;
    timing.metadata.stage = "after";
    timing.metadata.presentId = 1;
    timing.metadata.contractHash = 1;
    timing.metadata.outputWidth = 1920;
    timing.metadata.outputHeight = 1080;
    ImGui::click = "Measure NR choices###nrAdvisorStart";
    RenderAdvisor(&config, true);
    assert(!AdvisorRuntime::Active());
    config.timing.Enabled = false;
    RenderAdvisor(&config, false);
    assert(!AdvisorRuntime::Active());
    config.timing.Enabled = true;
    timing.sampleAgeMs = 1001;
    RenderAdvisor(&config, false);
    assert(!AdvisorRuntime::Active());
    timing.sampleAgeMs = 0;
    RenderAdvisor(&config, false);
    assert(AdvisorRuntime::Active() && configWrites == 0);
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
}
