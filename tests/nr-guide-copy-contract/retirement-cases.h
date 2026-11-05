// Runs the production collector and capture prefixes; the resource's Release asserts against outstanding GPU use.
int main()
{
    ID3D12Device device, other;
    ID3D12GraphicsCommandList cmd;
    ID3D12Resource depth, motion, retiredResource, oldClone;
    cmd.device = &device;
    depth.device = motion.device = &device;
    const auto original = depth.desc;
    auto arm = [&](int framesLeft = 1, size_t count = 1)
    {
        g_nrRetired.assign(count, NrRetired {});
        g_nrRetired[0].resource = &retiredResource;
        g_nrRetired[0].framesLeft = framesLeft;
        retiredResource.gpuOutstanding = true;
        retiredResource.releases = 0;
        lastReason = Reason::Recorded;
    };
    auto unchanged = [&]()
    {
        assert(!g_nrRetired.empty());
        assert(g_nrRetired[0].resource == &retiredResource);
        assert(g_nrRetired[0].framesLeft == 1);
        assert(retiredResource.releases == 0);
    };
    // Every early CPU rejection branch in CaptureGuides, including each member of compound guards.
    for (int which = 0; which < 18; ++which)
    {
        arm();
        depth.desc = motion.desc = original;
        depth.device = motion.device = &device;
        cmd.device = &device;
        device.failFormatInfo = false;
        GuideCapture source { &depth, &motion };
        auto* list = &cmd;
        Reason expected = Reason::Incompatible;
        switch (which)
        {
        case 0:
            list = nullptr;
            expected = Reason::MissingInput;
            break;
        case 1:
            source.depth = nullptr;
            expected = Reason::MissingInput;
            break;
        case 2:
            source.motion = nullptr;
            expected = Reason::MissingInput;
            break;
        case 3:
            depth.device = nullptr;
            expected = Reason::Device;
            break;
        case 4:
            cmd.device = &other;
            expected = Reason::Device;
            break;
        case 5:
            motion.device = &other;
            expected = Reason::Device;
            break;
        case 6:
            depth.desc.Dimension = 0;
            break;
        case 7:
            motion.desc.Dimension = 0;
            break;
        case 8:
            depth.desc.DepthOrArraySize = 2;
            break;
        case 9:
            motion.desc.DepthOrArraySize = 2;
            break;
        case 10:
            depth.desc.SampleDesc.Count = 2;
            break;
        case 11:
            motion.desc.SampleDesc.Count = 2;
            break;
        case 12:
            device.failFormatInfo = true;
            break;
        case 13:
            depth.desc.MipLevels = 0;
            break;
        case 14:
            motion.desc.MipLevels = 0;
            break;
        case 15:
            depth.desc.SampleDesc.Quality = 1;
            break;
        case 16:
            motion.desc.SampleDesc.Quality = 1;
            break;
        case 17:
            g_nr.failed = true;
            expected = Reason::Recorded;
            break;
        }
        assert(!CaptureGuides(list, source, {}));
        assert(lastReason == expected);
        unchanged();
        g_nr.failed = false;
    }
    depth.desc = motion.desc = original;
    depth.device = motion.device = &device;
    cmd.device = &device;
    device.failFormatInfo = false;
    for (int which = 0; which < 3; ++which)
    {
        arm();
        NVSDK_NGX_Parameter params { &depth, &motion };
        if (which == 0)
            params.depth = nullptr;
        if (which == 1)
            params.motion = nullptr;
        CaptureTemporal(which == 2 ? nullptr : &cmd, &params, {});
        assert(lastReason == Reason::MissingInput);
        unchanged();
    }
    arm();
    assert(!CaptureBridgedGuides(&cmd, &depth, &motion, 0, nullptr, 640, 480));
    assert(lastReason == Reason::MissingInput);
    unchanged();

    // A pure capacity query must never spend even one legacy grace tick.
    for (size_t count : { 1u, 31u, 32u })
    {
        arm(1, count);
        assert(PresentGuideRetirementCapacity() == (count < 32));
        unchanged();
    }
    // Normal capture still spends exactly the base commit's one tick. At the replacement check the
    // resource has framesLeft=1 with outstanding GPU use: a second tick would Release it prematurely.
    for (size_t count : { 1u, 32u })
    {
        arm(2, count);
        g_presentGuideClones[0].depth = &oldClone; // shape mismatch forces the real capacity branch
        const bool admitted = CaptureGuides(&cmd, { &depth, &motion }, {});
        assert(admitted == (count < 32));
        assert(lastReason == (admitted ? Reason::Recorded : Reason::RetiredCap));
        unchanged();
    }
    // Positive control: the real legacy collector still releases once its original countdown expires.
    arm();
    retiredResource.gpuOutstanding = false;
    TickNrRetired();
    assert(g_nrRetired.empty() && retiredResource.releases == 1);
    return 0;
}
