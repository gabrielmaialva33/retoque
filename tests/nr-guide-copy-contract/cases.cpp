#include <string>
int main(int argc, char** argv)
{
    assert(argc == 2);
    const std::string name = argv[1];
    ID3D12Device device;
    ID3D12GraphicsCommandList cmd;
    cmd.device = &device;
    ID3D12Resource source;
    source.device = &device;
    ID3D12Resource* clone = nullptr;
    if (name == "shape")
    {
        for (unsigned change = 0; change < 7; ++change)
        {
            source.desc = {};
            assert(ReadableGuide(&device, &cmd, &source, &clone) == clone);
            auto* previous = clone;
            const auto parks = retired.size();
            const auto count = allocations;
            switch (change)
            {
            case 0:
                source.desc.MipLevels = 4;
                break;
            case 1:
                source.desc.DepthOrArraySize = 3;
                break;
            case 2:
                source.desc.SampleDesc.Count = 4;
                break;
            case 3:
                source.desc.SampleDesc.Quality = 2;
                break;
            case 4:
                source.desc.Dimension = 4;
                break;
            case 5:
                source.desc.Width = 1280;
                break;
            case 6:
                source.desc.Height = 720;
                break;
            }
            assert(ReadableGuide(&device, &cmd, &source, &clone) == clone);
            assert(clone != previous && retired.size() == parks + 1 && allocations == count + 1);
            delete clone;
            clone = nullptr;
        }
    }
    else if (name == "format-change")
    {
        ReadableGuide(&device, &cmd, &source, &clone);
        auto* previous = clone;
        source.desc.Format = DXGI_FORMAT_R16_TYPELESS;
        auto* readable = ReadableGuide(&device, &cmd, &source, &clone);
        assert(readable && ExpectedFamily(readable->desc.Format) == 2);
        assert(clone != previous && retired.size() == 1 && retired.back().get() == previous);
    }
    else if (name == "fallback-family-r24" || name == "fallback-family-r32g8")
    {
        source.desc.Format = name == "fallback-family-r24" ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_R32G8X24_TYPELESS;
        device.allowedOnly = DXGI_FORMAT_R32_FLOAT;
        clone = CreateGuideClone(&device, &source);
        assert(clone == nullptr && "unsupported family must not become R32_FLOAT by bit copy");
        for (auto format : device.attemptedFormats)
            assert(format != DXGI_FORMAT_R32_FLOAT);
    }
    else if (name == "legal-fallback")
    {
        device.failedFormats.insert(DXGI_FORMAT_R32_FLOAT);
        assert(ReadableGuide(&device, &cmd, &source, &clone) == clone && clone);
        assert(clone->desc.Format == DXGI_FORMAT_D32_FLOAT);
        const auto count = allocations;
        source.desc.Alignment = 65536;
        source.desc.Flags = 0x400;
        for (unsigned frame = 0; frame < 3; ++frame)
            assert(ReadableGuide(&device, &cmd, &source, &clone) == clone);
        assert(allocations == count && retired.empty());
    }
    else if (name == "plane-query-failure" || name == "plane-mismatch")
    {
        device.failFormatInfo = name == "plane-query-failure";
        device.wrongPlanes = name == "plane-mismatch";
        assert(ReadableGuide(&device, &cmd, &source, &clone) == nullptr);
        assert(cmd.copies == 0 && cmd.barriers == 0);
    }
    else if (name == "same-family-two-planes")
    {
        source.desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
        assert(ReadableGuide(&device, &cmd, &source, &clone) == clone && clone);
        assert(clone->desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT && cmd.copies == 1);
    }
    else if (name == "device-change")
    {
        ReadableGuide(&device, &cmd, &source, &clone);
        auto* previous = clone;
        ID3D12Device other;
        source.device = &other;
        cmd.device = &other;
        auto* readable = ReadableGuide(&other, &cmd, &source, &clone);
        assert(readable && readable->device == &other);
        assert(clone != previous && retired.size() == 1 && retired.back().get() == previous);
    }
    else if (name == "mixed-device")
    {
        ID3D12Device other;
        source.device = &other;
        assert(ReadableGuide(&device, &cmd, &source, &clone) == nullptr);
        assert(allocations == 0 && cmd.copies == 0 && cmd.barriers == 0);
    }
    // 2026-10-30: a wrapper layer's list (ReShade through LoadReShade) on the device the game's guide reports. The
    // clone is made and copied through the wrapper, kept across frames, and a guide on another adapter is refused.
    else if (name == "wrapped-device")
    {
        ID3D12Device wrapper;
        wrapper.luid = device.luid;
        wrapper.wraps = &device;
        cmd.device = &wrapper;
        auto* readable = ReadableGuide(&wrapper, &cmd, &source, &clone);
        assert(readable && readable == clone && cmd.copies == 1);
        const auto count = allocations;
        for (unsigned frame = 0; frame < 3; ++frame)
            assert(ReadableGuide(&wrapper, &cmd, &source, &clone) == clone);
        assert(allocations == count && retired.empty() && cmd.copies == 4);
        ID3D12Device elsewhere;
        ID3D12Resource foreign;
        foreign.device = &elsewhere;
        ID3D12Resource* other = nullptr;
        assert(ReadableGuide(&wrapper, &cmd, &foreign, &other) == nullptr && other == nullptr);
        assert(wrapper.refs == 1 && device.refs == 1 && elsewhere.refs == 1);
    }
    else if (name == "allocation-and-passthrough")
    {
        device.failAllocation = true;
        assert(ReadableGuide(&device, &cmd, &source, &clone) == nullptr && cmd.copies == 0);
        source.desc.Format = DXGI_FORMAT_R32_FLOAT;
        assert(ReadableGuide(&device, &cmd, &source, &clone) == &source && cmd.copies == 0);
        assert(ReadableGuide(&device, &cmd, nullptr, &clone) == nullptr);
    }
    else
        assert(false && "unknown case");
    delete clone;
    std::cout << "PASS: " << name << '\n';
}
