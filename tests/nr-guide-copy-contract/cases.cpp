
int main()
{
    unsigned cases = 0;
    for (unsigned change = 0; change < 7; ++change)
    {
        ID3D12Device device;
        ID3D12GraphicsCommandList cmd;
        ID3D12Resource source;
        ID3D12Resource* clone = nullptr;
        ReadableGuide(&device, &cmd, &source, &clone);
        const auto previous = clone;
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
        auto* answer = ReadableGuide(&device, &cmd, &source, &clone);
        assert(answer == clone && clone != previous);
        assert(retired.size() == parks + 1 && retired.back().get() == previous);
        assert(allocations == count + 1 && cmd.copies == 2);
        // Same source shape, even with a fallback format and source-only flags/alignment.
        source.desc.Flags = 0x400;
        source.desc.Alignment = 65536;
        for (unsigned frame = 0; frame < 3; ++frame)
            assert(ReadableGuide(&device, &cmd, &source, &clone) == clone);
        assert(allocations == count + 1 && retired.size() == parks + 1);
        delete clone;
        ++cases;
    }
    ID3D12Device device;
    ID3D12GraphicsCommandList cmd;
    ID3D12Resource source;
    ID3D12Resource* clone = nullptr;
    failAllocation = true;
    assert(ReadableGuide(&device, &cmd, &source, &clone) == nullptr && cmd.copies == 0);
    failAllocation = false;
    ++cases;
    source.desc.Format = 1;
    assert(ReadableGuide(&device, &cmd, &source, &clone) == &source && cmd.copies == 0);
    assert(ReadableGuide(&device, &cmd, nullptr, &clone) == nullptr);
    ++cases;
    std::cout << "Guide copy contract: " << cases << " cases passed\n";
}
