#include "pch.h"
#include "../dlssnr-loopback/mask_matrix.h"
#include <cassert>
#include <iostream>

constexpr int DXGI_FORMAT_R8_UNORM = 61;
constexpr int DXGI_FORMAT_R8G8B8A8_UNORM = 28;
void Say(const char*, ...) {}
void Check(int result, const char*) { assert(result == 0); }
struct Queue
{
    int Signal(void*, unsigned) { return 0; }
};
struct Host
{
    unsigned width = 0, height = 0, serial = 0, useAutoMask = 0;
    unsigned generations = 0, frames = 0, recorded = 0, state = 0;
    bool allocated = false, maskSubrect = false, maskResourceSetter = false, maskLogged = true;
    int maskPattern = 0, maskFormat = 0, maskBlendMode = -1;
    float skinStructure = 0, intensity = 0, localTone = 0, localStructure = 0;
    std::string trial;
    std::array<unsigned char, 4> maskInside {}, maskOutside {};
    void CoverHud() { assert(width == 1280 && height == 720); }
    void Allocate(unsigned w, unsigned h)
    {
        assert(!allocated && w == 1280 && h == 720);
        const auto expected = MaskMatrix::Trials().at(generations);
        assert(trial == expected.name && maskPattern == (expected.masked ? 3 : -1));
        assert(maskInside == expected.rgba && maskOutside == expected.rgba);
        assert(maskFormat == (expected.r8 ? 61 : 28) && maskSubrect);
        assert(maskResourceSetter == expected.resourceSetter && maskBlendMode == int(expected.blend));
        assert(!maskLogged && useAutoMask == 1 && skinStructure == -1);
        assert(intensity == 1 && localTone == 1 && localStructure == 1);
        allocated = true;
        frames = 0;
    }
    void Record(unsigned frame)
    {
        assert(allocated && state == 0 && frame == frames);
        ++recorded;
        state = 1;
    }
    void Submit()
    {
        assert(state == 1);
        state = 2;
    }
    void ReadTiming(unsigned frame)
    {
        assert(state == 2 && frame == frames);
        state = 3;
    }
    void Inspect(unsigned frame)
    {
        assert(state == 3 && frame == frames);
        state = 4;
    }
    void Present()
    {
        assert(state == 4);
        state = 0;
        ++frames;
    }
    void ReleaseGeneration()
    {
        assert(allocated && state == 0 && frames == 32 && serial == generations + 1);
        allocated = false;
        ++generations;
    }
};
int main()
{
    Host host;
    Queue storage;
    Queue* queue = &storage;
    void* fence = nullptr;
#include "full_branch.inc"
    assert(!host.allocated && host.generations == 184 && host.recorded == 5888);
    std::cout << host.generations << ' ' << host.recorded << '\n';
}
