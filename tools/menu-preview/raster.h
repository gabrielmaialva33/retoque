#pragma once
// Original CPU rasterizer for ImGui triangles. No window or graphics API.
// Pixel-centre samples, top-left edge ownership, bilinear font-atlas filtering and source-over blending.
namespace Preview
{
struct Raster
{
    int width, height, atlasWidth, atlasHeight;
    const unsigned char* atlas;
    std::vector<uint8_t> rgb;
    size_t glyphPixels = 0, triangles = 0;
    Raster(int w, int h)
        : width(w), height(h), atlasWidth(0), atlasHeight(0), atlas(nullptr), rgb(size_t(w) * h * 3, 24)
    {
    }

    static double Edge(ImVec2 a, ImVec2 b, ImVec2 p)
    {
        return double(b.x - a.x) * (p.y - a.y) - double(b.y - a.y) * (p.x - a.x);
    }
    static bool TopLeft(ImVec2 a, ImVec2 b) { return b.y < a.y || (b.y == a.y && b.x > a.x); }
    double Sample(double u, double v, int channel) const
    {
        const double x = u * atlasWidth - 0.5, y = v * atlasHeight - 0.5;
        const int ix = int(std::floor(x)), iy = int(std::floor(y));
        const double fx = x - ix, fy = y - iy;
        auto at = [&](int px, int py)
        {
            return atlas[(size_t(std::clamp(py, 0, atlasHeight - 1)) * atlasWidth + std::clamp(px, 0, atlasWidth - 1)) *
                             4 +
                         channel] /
                   255.0;
        };
        return (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy) +
               (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy;
    }
    void Triangle(ImDrawVert a, ImDrawVert b, ImDrawVert c, ImVec4 clip)
    {
        double area = Edge(a.pos, b.pos, c.pos);
        if (area == 0)
            return;
        if (area < 0)
        {
            std::swap(b, c);
            area = -area;
        }
        ++triangles;
        const bool textured = a.uv.x != b.uv.x || a.uv.y != b.uv.y || a.uv.x != c.uv.x || a.uv.y != c.uv.y;
        const int left =
            std::max({ 0, int(std::ceil(clip.x)), int(std::floor(std::min({ a.pos.x, b.pos.x, c.pos.x }))) });
        const int top =
            std::max({ 0, int(std::ceil(clip.y)), int(std::floor(std::min({ a.pos.y, b.pos.y, c.pos.y }))) });
        const int right =
            std::min({ width, int(std::floor(clip.z)), int(std::ceil(std::max({ a.pos.x, b.pos.x, c.pos.x }))) });
        const int bottom =
            std::min({ height, int(std::floor(clip.w)), int(std::ceil(std::max({ a.pos.y, b.pos.y, c.pos.y }))) });
        for (int y = top; y < bottom; ++y)
            for (int x = left; x < right; ++x)
            {
                ImVec2 p(x + 0.5f, y + 0.5f);
                double e0 = Edge(b.pos, c.pos, p), e1 = Edge(c.pos, a.pos, p), e2 = Edge(a.pos, b.pos, p);
                if (e0 < 0 || (e0 == 0 && !TopLeft(b.pos, c.pos)) || e1 < 0 || (e1 == 0 && !TopLeft(c.pos, a.pos)) ||
                    e2 < 0 || (e2 == 0 && !TopLeft(a.pos, b.pos)))
                    continue;
                const double w0 = e0 / area, w1 = e1 / area, w2 = e2 / area;
                const double u = w0 * a.uv.x + w1 * b.uv.x + w2 * c.uv.x, v = w0 * a.uv.y + w1 * b.uv.y + w2 * c.uv.y;
                auto color = [&](int shift)
                {
                    return (w0 * ((a.col >> shift) & 255) + w1 * ((b.col >> shift) & 255) +
                            w2 * ((c.col >> shift) & 255)) /
                           255.0;
                };
                const double alpha = color(IM_COL32_A_SHIFT) * Sample(u, v, 3);
                if (textured && alpha > 0.1)
                    ++glyphPixels;
                const int shifts[] = { IM_COL32_R_SHIFT, IM_COL32_G_SHIFT, IM_COL32_B_SHIFT };
                for (int channel = 0; channel < 3; ++channel)
                {
                    auto& dst = rgb[(size_t(y) * width + x) * 3 + channel];
                    dst = uint8_t(std::clamp(
                        std::lround(255 * color(shifts[channel]) * Sample(u, v, channel) * alpha + dst * (1 - alpha)),
                        0l, 255l));
                }
            }
    }
    void Draw(const ImDrawData& data)
    {
        if (data.DisplayPos.x != 0 || data.DisplayPos.y != 0 || data.FramebufferScale.x != 1 ||
            data.FramebufferScale.y != 1)
            throw std::runtime_error("Preview requires origin zero and framebuffer scale one");
        for (auto* list : data.CmdLists)
            for (const auto& cmd : list->CmdBuffer)
            {
                if (cmd.UserCallback != nullptr || cmd.GetTexID() != 1 || cmd.ElemCount % 3 != 0)
                    throw std::runtime_error("Unsupported ImGui draw command");
                const auto* texture = cmd.TexRef._TexData;
                if (!texture || texture->Format != ImTextureFormat_RGBA32 || !texture->Pixels)
                    throw std::runtime_error("Expected RGBA font-atlas pixels");
                atlas = texture->Pixels;
                atlasWidth = texture->Width;
                atlasHeight = texture->Height;
                for (unsigned i = 0; i < cmd.ElemCount; i += 3)
                {
                    auto vertex = [&](unsigned k)
                    { return list->VtxBuffer[list->IdxBuffer[cmd.IdxOffset + i + k] + cmd.VtxOffset]; };
                    Triangle(vertex(0), vertex(1), vertex(2), cmd.ClipRect);
                }
            }
    }
    void Save(const std::filesystem::path& path) const
    {
        std::ofstream file(path, std::ios::binary);
        auto sink = [&](const uint8_t* p, size_t n)
        {
            file.write(reinterpret_cast<const char*>(p), n);
            return bool(file);
        };
        DlssNr::ImageFile::PngStream stream(sink);
        if (!stream.Begin(width, height, 8, true, "Description",
                          "Synthetic Retoque menu preview; no game, GPU, real values or timing"))
            throw std::runtime_error("PNG header failed");
        for (int y = 0; y < height; ++y)
            if (!stream.Row(rgb.data() + size_t(y) * width * 3))
                throw std::runtime_error("PNG row failed");
        if (!stream.End())
            throw std::runtime_error("PNG end failed");
        file.close();
        if (!file)
            throw std::runtime_error("PNG write failed");
    }
};
} // namespace Preview
