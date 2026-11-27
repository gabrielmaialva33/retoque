int main()
{
    assert(!SlSourceRetirementCapacity(0));
    assert(SlSourceRetirementCapacity(2));
    g_nrRetired.resize(31);
    assert(!SlSourceRetirementCapacity(2));
    g_nrRetired.resize(30);
    assert(SlSourceRetirementCapacity(2));
    g_nrRetired.resize(64);
    for (auto& r : g_nrRetired)
        r.stuck = true;
    assert(SlSourceRetirementCapacity(4));
    g_nrRetired.clear();
    ID3D12Device device;
    ID3D12CommandQueue queue { &device };
    IDXGISwapChain3 chain;
    PresentGuideIdentity::Present observed;
    assert(!ObserveSlPresentBoundary(&chain, &queue, observed));
    assert(g_guideIdentity.tracker.Epoch() == 0);
    Config::Instance()->DlssNrPresentSlGuides.value = true;
    assert(ObserveSlPresentBoundary(&chain, &queue, observed));
    assert(observed.epoch == 1 && g_presentGuideRing.NewestRecordedSequence() == 0);
    auto constants = []
    {
        sl::Constants c;
        c.depthInverted = sl::Boolean::eTrue;
        c.motionVectorsJittered = sl::Boolean::eTrue;
        c.cameraMotionIncluded = sl::Boolean::eTrue;
        c.motionVectors3D = sl::Boolean::eFalse;
        c.mvecScale = { 1.0f / 1280, -1.0f / 720 };
        c.jitterOffset = { 0.25f, -0.25f };
        return c;
    };
    auto c = constants();
    ID3D12Resource d, m;
    m.desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    sl::Resource rd, rm;
    rd.native = &d;
    rm.native = &m;
    rd.state = rm.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    sl::ResourceTag pair[] { { &rd, sl::kBufferTypeDepth, sl::eOnlyValidNow },
                             { &rm, sl::kBufferTypeMotionVectors, sl::eOnlyValidNow } };
    ID3D12GraphicsCommandList list;
    SlPresent::Constants(c, 1, 0);
    SlPresent::Tags(1, 0, pair, 2, &list, true, true);
    assert(copies == 1 && lastFrame.DepthInverted && lastFrame.MotionVectorsJittered);
    assert(lastFrame.MvScaleX == 1 && lastFrame.MvScaleY == -1 && lastFrame.JitterOffsetX == .25f);
    SlPresent::MarkerStart(1);
    assert(ObserveSlPresentBoundary(&chain, &queue, observed));
    assert(SlPresentSlotEligible(&writeToken, true));
    assert(!SlPresentSlotEligible(&writeToken, false));
    sl::ResourceTag hudless { nullptr, sl::kBufferTypeHUDLessColor, sl::eOnlyValidNow };
    SlPresent::Tags(1, 0, &hudless, 1, &list, true, true);
    assert(copies == 1 && SlPresentSlotEligible(&writeToken, true)); // HUD-less refusal never poisons depth/motion

    auto ask = AskObservedPresentGuides(observed, &device, &chain, 1920, 1080, 0);
    assert(ask.decision.reason == PresentGuideIdentity::Reason::Accepted);
    assert(g_guideIdentity.tracker.Epoch() == 2);
    SlPresent::Tags(1, 0, pair, 1, &list, true, true); // removal/replacement/incomplete retag poisons the copied pair
    assert(!SlPresentSlotEligible(&writeToken, true));
    for (int scenario = 0; scenario < 15; ++scenario)
    {
        slGuides.frames = {};
        const auto before = copies;
        auto current = constants();
        pair[0].lifecycle = sl::eOnlyValidNow;
        pair[0].extent = {};
        rd.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        list.safe = true;
        if (scenario == 0)
            current.cameraMotionIncluded = sl::Boolean::eFalse;
        if (scenario == 1)
            current.motionVectors3D = sl::Boolean::eTrue;
        if (scenario == 2)
            current.mvecScale.x = 0;
        if (scenario == 3)
            current.depthInverted = sl::Boolean::eInvalid;
        if (scenario == 4)
            pair[0].lifecycle = sl::eValidUntilPresent;
        if (scenario == 5)
            pair[0].lifecycle = sl::eValidUntilEvaluate;
        if (scenario == 6)
            pair[0].extent.left = 1;
        if (scenario == 7)
            rd.state = UINT_MAX;
        if (scenario == 8)
            list.safe = false;
        if (scenario == 12)
            current.jitterOffset.x = sl::INVALID_FLOAT;
        if (scenario == 13)
            current.mvecScale.y = sl::INVALID_FLOAT;
        if (scenario == 14)
            current.jitterOffset.y = INFINITY;
        SlPresent::Constants(current, 10, 0);
        SlPresent::Tags(10, 0, pair, scenario == 11 ? 1 : 2, &list, scenario != 9, scenario != 10);
        assert(copies == before);
    }
    slGuides.frames = {};
    pair[0].lifecycle = sl::eOnlyValidNow;
    pair[0].extent = {};
    list.safe = true;
    rd.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    SlPresent::Constants(c, 20, 0);
    SlPresent::Tags(20, 0, pair, 2, &list, true, true);
    assert(copies == 2);
    // A current upscaler capture has priority even if the tagged one is newer/otherwise eligible.
    auto stamp = g_guideIdentity.tracker.Stamped()[0];
    stamp.streamline = false;
    g_guideIdentity.tracker.StampCapture(1, stamp, 100);
    g_presentGuideRing.CommitRecord(1, 1280, 720, false, 100, &otherWrite, nullptr);
    SlPresent::MarkerStart(20);
    ObserveSlPresentBoundary(&chain, &queue, observed);
    assert(!SlPresentSlotEligible(&writeToken, true));
    assert(SlPresentSlotEligible(&otherWrite, true));
    IDXGISwapChain3 second;
    ObserveSlPresentBoundary(&second, &queue, observed);
    assert(slGuides.ambiguous); // no age-based restoration of a previously ambiguous owner
    puts("PASS: production constants/tags/real-boundary/eligibility adapter against portable SDK and D3D12 fakes");
}
