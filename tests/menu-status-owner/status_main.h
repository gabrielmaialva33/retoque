#pragma once
static void CheckStatusCollector()
{
    Config config;
    using namespace DlssNr;
    RenderStatus(&config);
    assert(reads == 0 && ImGui::values == std::vector<std::string> { "Off" });
    ImGui::values.clear();
    config.DlssNrEnabled.value = true;
    auto in = CollectStatusInputs(&config);
    assert(in.failure == "test failure" && in.model.empty());
    assert(in.passes == 3 && in.cadence == 2 && in.scale == 0.5f && in.rrScale == 0.75f);
    dxRunning = true;
    in = CollectStatusInputs(&config);
    assert(in.failure.empty() && in.backend == StatusPage::Backend::D3D12);
    assert(in.cadenceStatus == "cadence declined");
    dxRoute = Dx11Route::Report::NoPass;
    in = CollectStatusInputs(&config);
    assert(in.hideHook && !in.route.empty());
    for (const auto& row : StatusPage::Rows(in))
        assert(row.value != "upscaled: active");
    sceneStatus = "native scene transport";
    assert(CollectStatusInputs(&config).route == sceneStatus);
    vkRunning = true;
    in = CollectStatusInputs(&config);
    assert(in.backend == StatusPage::Backend::Vulkan && in.cadenceStatus.empty());
    config.model = "C:\\Users\\Example\\model.dll";
    in = CollectStatusInputs(&config);
    assert(StatusPage::ModelText(in.model) == "model.dll (startup selection)");
    DriverGate::Publish(DriverGate::Verdict::TooOld, 57688);
    assert(CollectStatusInputs(&config).failure == DriverGate::kTooOldReason);
    ImGui::table = false;
    const int previousReads = reads, previousEnds = ImGui::ends;
    RenderStatus(&config);
    assert(reads == previousReads && ImGui::ends == previousEnds);
}
