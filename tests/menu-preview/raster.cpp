#include "pch.h"
#include <dlssnr/DlssNr_ImageFile.h>
#include "raster.h"
#include <cassert>

int main()
{
    const unsigned char white[] = { 255, 255, 255, 255 };
    Preview::Raster r(4, 4);
    r.atlas = white;
    r.atlasWidth = r.atlasHeight = 1;
    const auto vertex = [](float x, float y)
    { return ImDrawVert { ImVec2(x, y), ImVec2(0.5f, 0.5f), IM_COL32(255, 255, 255, 128) }; };
    const ImVec4 clip(0, 0, 4, 4);
    r.Triangle(vertex(0, 0), vertex(4, 0), vertex(4, 4), clip);
    r.Triangle(vertex(0, 0), vertex(4, 4), vertex(0, 4), clip);
    // Every centre, including the shared diagonal, is blended exactly once: round(255*128/255+24*127/255).
    for (auto c : r.rgb)
        assert(c == 140);
    Preview::Raster clipped(4, 4);
    clipped.atlas = white;
    clipped.atlasWidth = clipped.atlasHeight = 1;
    clipped.Triangle(vertex(0, 0), vertex(4, 0), vertex(4, 4), ImVec4(1, 1, 3, 3));
    clipped.Triangle(vertex(0, 0), vertex(4, 4), vertex(0, 4), ImVec4(1, 1, 3, 3));
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            assert(clipped.rgb[(y * 4 + x) * 3] == (x >= 1 && x < 3 && y >= 1 && y < 3 ? 140 : 24));
    const unsigned char checker[] = { 255, 255, 255, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0 };
    r.atlas = checker;
    r.atlasWidth = r.atlasHeight = 2;
    assert(r.Sample(0.5, 0.5, 3) == 0.5);
    assert(r.Sample(0, 0, 3) == 0);
    assert(r.Sample(1, 0, 3) == 1);
    std::cout << "PASS: triangle edge ownership, source-over, clip and bilinear alpha\n";
}
