// The Vulkan route's guides below the frame's size (docs/dlssnr/design/reduced-scale-guides-vk.md): the production
// rules in retoque/shaders/dlssnr/DlssNr_GuideMatch.h (what the model is handed, and when the frame is refused) and
// DlssNr_GuideMatch_Vk.h (which of the game's views the resample may read, the outputs it needs, its push block and
// dispatch), compiled against the real Vulkan headers and pinned with the numbers of the shapes they are for. No Vulkan
// device: tests/guide-match-vk runs the pass itself.
#include <shaders/dlssnr/DlssNr_GuideMatch.h>
#include <shaders/dlssnr/DlssNr_GuideMatch_Vk.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace DlssNr::GuideMatch;
namespace Vkr = DlssNr::GuideMatch::Vk;

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

static bool Same(const Region& r, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    return r.baseX == x && r.baseY == y && r.width == w && r.height == h;
}

// The D3D12 route's own arithmetic (DlssNr_Dx12.cpp, the non-periphery branch), written out: the Vulkan plan must give
// the model exactly what D3D12 gives it for the same shape.
struct D3d12Says
{
    uint32_t motionWidth, motionHeight;
    float scaleX, scaleY;
};

static D3d12Says D3d12(bool matched, bool renderMotionScale, uint32_t motionWidth, uint32_t motionHeight,
                       uint32_t workWidth, uint32_t workHeight, uint32_t width, uint32_t height, uint32_t refWidth,
                       uint32_t refHeight, float gameX, float gameY)
{
    const uint32_t mw = matched ? workWidth : motionWidth;
    const uint32_t mh = matched ? workHeight : motionHeight;
    return { mw, mh,
             renderMotionScale ? ModelMotionScale(gameX, mw, refWidth) : LegacyMotionScale(gameX, workWidth, width),
             renderMotionScale ? ModelMotionScale(gameY, mh, refHeight)
                               : LegacyMotionScale(gameY, workHeight, height) };
}

static void PlanCases()
{
    // DOOM Eternal at DLSS Quality, 3440x1440 (render 2293x960, low-resolution vectors, the game's scale in render
    // pixels), on an RTX 20 or 30 card at WorkingScale=auto (0.5): a 1720x720 model. Both guides are larger than it.
    const Region depth { 0, 0, 2293, 960 };
    const Region motion { 0, 0, 2293, 960 };
    const uint32_t refW = MotionReference(true, 2293, 3440), refH = MotionReference(true, 960, 1440);
    CHECK(refW == 2293 && refH == 960);

    const bool wanted = Wanted(true, true, 1720, 720, depth.width, depth.height, motion.width, motion.height);
    CHECK(wanted);
    CHECK(Choose(wanted, true) == Guides::Matched);

    // Matched: a full zero-origin region at the working size for both, and the game's scale re-expressed for it.
    const Handed m = ForModel(true, true, depth, motion, 1720, 720, 3440, 1440, refW, refH, 2293.0f, 960.0f);
    CHECK(m.matched);
    CHECK(Same(m.depth, 0, 0, 1720, 720));
    CHECK(Same(m.motion, 0, 0, 1720, 720));
    CHECK(Near(m.scaleX, 1720.0f) && Near(m.scaleY, 720.0f));

    // What the route hands the model today for this shape, the fault: the game's 2293x960 for a 1720x720 colour.
    // The plan never gives that with the resample wanted -- it is matched or refused.
    CHECK(!(m.depth.width == 2293 && m.motion.width == 2293));

    // The resample wanted and unable to run: the frame is refused, never handed frame-size guides.
    CHECK(Choose(wanted, false) == Guides::Refused);

    // MatchGuides off is the A/B arm, as on D3D12: not wanted, so the game's regions, with fix 2's scale.
    const bool off = Wanted(false, true, 1720, 720, depth.width, depth.height, motion.width, motion.height);
    CHECK(!off && Choose(off, false) == Guides::Game && Choose(off, true) == Guides::Game);
    const Handed g = ForModel(false, true, depth, motion, 1720, 720, 3440, 1440, refW, refH, 2293.0f, 960.0f);
    CHECK(!g.matched && Same(g.depth, 0, 0, 2293, 960) && Same(g.motion, 0, 0, 2293, 960));
    CHECK(Near(g.scaleX, 2293.0f) && Near(g.scaleY, 960.0f));

    // RenderMotionScale off: the legacy working / frame conversion, matched or not (the A/B key).
    const Handed legacy = ForModel(true, false, depth, motion, 1720, 720, 3440, 1440, refW, refH, 2293.0f, 960.0f);
    CHECK(Same(legacy.motion, 0, 0, 1720, 720) && Near(legacy.scaleX, 1146.5f) && Near(legacy.scaleY, 480.0f));

    // Subrect bases: kept for the game's regions, dropped for the matched pair (it starts at its origin).
    const Region depthAt { 16, 8, 2293, 960 };
    const Region motionAt { 4, 2, 2293, 960 };
    const Handed based = ForModel(false, true, depthAt, motionAt, 1720, 720, 3440, 1440, refW, refH, 2293.0f, 960.0f);
    CHECK(Same(based.depth, 16, 8, 2293, 960) && Same(based.motion, 4, 2, 2293, 960));
    const Handed basedMatched =
        ForModel(true, true, depthAt, motionAt, 1720, 720, 3440, 1440, refW, refH, 2293.0f, 960.0f);
    CHECK(Same(basedMatched.depth, 0, 0, 1720, 720) && Same(basedMatched.motion, 0, 0, 1720, 720));

    // DLSS Performance at 0.5: render 1720x720 is the model's size. Nothing is larger, nothing to resample, and the
    // route is what it was.
    CHECK(!Wanted(true, true, 1720, 720, 1720, 720, 1720, 720));
    const Region perf { 0, 0, 1720, 720 };
    const Handed p = ForModel(false, true, perf, perf, 1720, 720, 3440, 1440, 1720, 720, 1720.0f, 720.0f);
    CHECK(Same(p.depth, 0, 0, 1720, 720) && Near(p.scaleX, 1720.0f) && Near(p.scaleY, 720.0f));

    // Display-resolution vectors (no MVLowRes): motion is frame-sized and measured against the frame. Both larger than
    // the model; matched, the scale shrinks with the texture.
    const Region full { 0, 0, 3440, 1440 };
    CHECK(MotionReference(false, 2293, 3440) == 3440);
    CHECK(Wanted(true, true, 1720, 720, 2293, 960, 3440, 1440));
    const Handed d = ForModel(true, true, depth, full, 1720, 720, 3440, 1440, 3440, 1440, 1.0f, 1.0f);
    CHECK(Same(d.motion, 0, 0, 1720, 720) && Near(d.scaleX, 0.5f) && Near(d.scaleY, 0.5f));

    // Supersampling 2x: the model is larger than every guide. Never resampled, never refused.
    CHECK(!Wanted(true, true, 6880, 2880, 2293, 960, 2293, 960));
    CHECK(Choose(false, false) == Guides::Game);

    // One axis is enough, as on D3D12.
    CHECK(Wanted(true, true, 1720, 720, 1700, 800, 1700, 700));

    // The route harness's shape (tests/vulkan-present-host nr-route-guides): 960x540 -> 1280x720 at 0.5, a 640x360
    // model under 960x540 guides.
    CHECK(Wanted(true, true, 640, 360, 960, 540, 960, 540));
    const Region harness { 0, 0, 960, 540 };
    const Handed h = ForModel(true, true, harness, harness, 640, 360, 1280, 720, 960, 540, 1.0f, 1.0f);
    CHECK(Same(h.depth, 0, 0, 640, 360) && Near(h.scaleX, 640.0f / 960.0f) && Near(h.scaleY, 360.0f / 540.0f));

    // For every shape here, matched or not, both scale keys: what the Vulkan plan hands the model is what the D3D12
    // route hands it (the motion texture's extent and the scale).
    struct Shape
    {
        uint32_t workW, workH, frameW, frameH, guideW, guideH;
        bool lowRes;
        float gameX, gameY;
    };
    const Shape shapes[] = {
        { 1720, 720, 3440, 1440, 2293, 960, true, 2293.0f, 960.0f },
        { 1720, 720, 3440, 1440, 2293, 960, false, 1.0f, 1.0f },
        { 960, 540, 1920, 1080, 1280, 720, true, -1280.0f, 720.0f },
        { 1344, 756, 2688, 1512, 1920, 1080, true, 1920.0f, 1080.0f },
        { 2688, 1512, 3840, 2160, 1920, 1080, true, 1920.0f, 1080.0f },
        { 6880, 2880, 3440, 1440, 2293, 960, true, 2293.0f, 960.0f },
    };
    for (const Shape& s : shapes)
    {
        const uint32_t mw = s.lowRes ? s.guideW : s.frameW, mh = s.lowRes ? s.guideH : s.frameH;
        const uint32_t rw = MotionReference(s.lowRes, s.guideW, s.frameW);
        const uint32_t rh = MotionReference(s.lowRes, s.guideH, s.frameH);
        const bool want = Wanted(true, true, s.workW, s.workH, s.guideW, s.guideH, mw, mh);
        for (const bool rms : { true, false })
        {
            const Handed v = ForModel(want, rms, Region { 0, 0, s.guideW, s.guideH }, Region { 0, 0, mw, mh }, s.workW,
                                      s.workH, s.frameW, s.frameH, rw, rh, s.gameX, s.gameY);
            const D3d12Says x =
                D3d12(want, rms, mw, mh, s.workW, s.workH, s.frameW, s.frameH, rw, rh, s.gameX, s.gameY);
            CHECK(v.matched == want);
            CHECK(v.motion.width == x.motionWidth && v.motion.height == x.motionHeight);
            CHECK(v.scaleX == x.scaleX && v.scaleY == x.scaleY);
            CHECK(!want || (Same(v.depth, 0, 0, s.workW, s.workH) && Same(v.motion, 0, 0, s.workW, s.workH)));
        }
    }

    // The point sample the shader repeats, at DOOM's 2293 -> 1720 with a base: inside the region and in order.
    uint32_t previous = 0;
    for (uint32_t i = 0; i < 1720; ++i)
    {
        const uint32_t s = PointSource(i, 1720, 16, 2293);
        CHECK(s >= 16 && s < 16 + 2293);
        CHECK(i == 0 || s >= previous);
        previous = s;
    }
}

static void ViewCases()
{
    constexpr VkFormatFeatureFlags sampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    constexpr VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    constexpr VkImageAspectFlags both = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    constexpr VkImageAspectFlags colour = VK_IMAGE_ASPECT_COLOR_BIT;

    // Depth: a depth-only format, a depth view of a depth/stencil format, a one-channel colour copy.
    CHECK(Vkr::DepthViewRefusal(VK_FORMAT_D32_SFLOAT, depthAspect, 1, sampled) == nullptr);
    CHECK(Vkr::DepthViewRefusal(VK_FORMAT_D16_UNORM, depthAspect, 1, sampled) == nullptr);
    CHECK(Vkr::DepthViewRefusal(VK_FORMAT_X8_D24_UNORM_PACK32, depthAspect, 1, sampled) == nullptr);
    CHECK(Vkr::DepthViewRefusal(VK_FORMAT_D24_UNORM_S8_UINT, depthAspect, 1, sampled) == nullptr);
    CHECK(Vkr::DepthViewRefusal(VK_FORMAT_D32_SFLOAT_S8_UINT, depthAspect, 1, sampled) == nullptr);
    CHECK(Vkr::DepthViewRefusal(VK_FORMAT_R32_SFLOAT, colour, 1, sampled) == nullptr);
    CHECK(Vkr::DepthViewRefusal(VK_FORMAT_R16_SFLOAT, colour, 1, sampled) == nullptr);
    // A depth-only format has one aspect whatever the game wrote; a remaining-layers count is one layer on a 2D view.
    CHECK(Vkr::DepthViewRefusal(VK_FORMAT_D32_SFLOAT, 0, VK_REMAINING_ARRAY_LAYERS, sampled) == nullptr);

    // Refused: both aspects named (VUID-VkDescriptorImageInfo-imageView-01976), a depth/stencil format whose aspect
    // is not stated, stencil alone, an integer format, more than one layer, no sampled support, an unknown format.
    CHECK(Vkr::DepthViewRefusal(VK_FORMAT_D24_UNORM_S8_UINT, both, 1, sampled) != nullptr);
    CHECK(Vkr::DepthViewRefusal(VK_FORMAT_D24_UNORM_S8_UINT, 0, 1, sampled) != nullptr);
    CHECK(Vkr::DepthViewRefusal(VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_STENCIL_BIT, 1, sampled) != nullptr);
    CHECK(Vkr::DepthViewRefusal(VK_FORMAT_S8_UINT, VK_IMAGE_ASPECT_STENCIL_BIT, 1, sampled) != nullptr);
    CHECK(Vkr::DepthViewRefusal(VK_FORMAT_R32_UINT, colour, 1, sampled) != nullptr);
    CHECK(Vkr::DepthViewRefusal(VK_FORMAT_D32_SFLOAT, depthAspect, 2, sampled) != nullptr);
    CHECK(Vkr::DepthViewRefusal(VK_FORMAT_D32_SFLOAT, depthAspect, 1, 0) != nullptr);
    CHECK(Vkr::DepthViewRefusal(VK_FORMAT_UNDEFINED, colour, 1, sampled) != nullptr);
    CHECK(Vkr::DepthViewRefusal(VK_FORMAT_R32_SFLOAT, depthAspect, 1, sampled) != nullptr);

    // Motion: two float components or more.
    CHECK(Vkr::MotionViewRefusal(VK_FORMAT_R16G16_SFLOAT, colour, 1, sampled) == nullptr);
    CHECK(Vkr::MotionViewRefusal(VK_FORMAT_R32G32_SFLOAT, colour, 1, sampled) == nullptr);
    CHECK(Vkr::MotionViewRefusal(VK_FORMAT_R16G16_UNORM, colour, 1, sampled) == nullptr);
    CHECK(Vkr::MotionViewRefusal(VK_FORMAT_R16G16_SNORM, colour, 1, sampled) == nullptr);
    CHECK(Vkr::MotionViewRefusal(VK_FORMAT_R16G16B16A16_SFLOAT, colour, 1, sampled) == nullptr);
    CHECK(Vkr::MotionViewRefusal(VK_FORMAT_R32G32B32A32_SFLOAT, 0, VK_REMAINING_ARRAY_LAYERS, sampled) == nullptr);
    CHECK(Vkr::MotionViewRefusal(VK_FORMAT_R16_SFLOAT, colour, 1, sampled) != nullptr);
    CHECK(Vkr::MotionViewRefusal(VK_FORMAT_R16G16_SINT, colour, 1, sampled) != nullptr);
    CHECK(Vkr::MotionViewRefusal(VK_FORMAT_R32G32_UINT, colour, 1, sampled) != nullptr);
    CHECK(Vkr::MotionViewRefusal(VK_FORMAT_D32_SFLOAT, depthAspect, 1, sampled) != nullptr);
    CHECK(Vkr::MotionViewRefusal(VK_FORMAT_R16G16_SFLOAT, colour, 6, sampled) != nullptr);
    CHECK(Vkr::MotionViewRefusal(VK_FORMAT_R16G16_SFLOAT, colour, 1, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != nullptr);

    // Every refusal says something.
    const char* reason = Vkr::DepthViewRefusal(VK_FORMAT_D24_UNORM_S8_UINT, both, 1, sampled);
    CHECK(reason != nullptr && std::strlen(reason) > 10);
}

static void OutputCases()
{
    // The outputs: D3D12's formats, stored and sampled, with the usage every route image is created with.
    CHECK(Vkr::kDepthFormat == VK_FORMAT_R32_SFLOAT);
    CHECK(Vkr::kMotionFormat == VK_FORMAT_R32G32_SFLOAT);
    CHECK(Vkr::kOutputUsage == (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));

    constexpr VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    CHECK(Vkr::OutputRefusal(need, true, need, true) == nullptr);
    CHECK(Vkr::OutputRefusal(need | VK_FORMAT_FEATURE_BLIT_SRC_BIT, true, need, true) == nullptr);
    CHECK(Vkr::OutputRefusal(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, true, need, true) != nullptr);
    CHECK(Vkr::OutputRefusal(need, true, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, true) != nullptr);
    CHECK(Vkr::OutputRefusal(need, false, need, true) != nullptr);
    CHECK(Vkr::OutputRefusal(need, true, need, false) != nullptr);
}

static void InterfaceCases()
{
    // Push constants, the Vulkan side of the D3D12 note's constant-buffer lesson: within the 128 bytes every device
    // guarantees (maxPushConstantsSize), a multiple of 4 from offset 0, the D3D12 cbuffer's twelve scalars in order.
    static_assert(sizeof(Vkr::Push) == 48);
    static_assert(sizeof(Vkr::Push) <= 128 && sizeof(Vkr::Push) % 4 == 0);
    static_assert(offsetof(Vkr::Push, DepthBaseX) == 0 && offsetof(Vkr::Push, MotionBaseX) == 16 &&
                  offsetof(Vkr::Push, OutWidth) == 32 && offsetof(Vkr::Push, Pad1) == 44);

    const Vkr::Push push = Vkr::MakePush(Region { 16, 8, 2293, 960 }, Region { 4, 2, 2294, 961 }, 1720, 720);
    CHECK(push.DepthBaseX == 16 && push.DepthBaseY == 8 && push.DepthWidth == 2293 && push.DepthHeight == 960);
    CHECK(push.MotionBaseX == 4 && push.MotionBaseY == 2 && push.MotionWidth == 2294 && push.MotionHeight == 961);
    CHECK(push.OutWidth == 1720 && push.OutHeight == 720 && push.Pad0 == 0 && push.Pad1 == 0);

    // 8x8 groups, 64 invocations (Vulkan guarantees 128), covering every texel of the working size.
    CHECK(Vkr::kGroup == 8);
    CHECK(Vkr::Groups(1720) == 215 && Vkr::Groups(720) == 90 && Vkr::Groups(721) == 91 && Vkr::Groups(1) == 1);
    CHECK(Vkr::Groups(0) == 0);

    // The descriptor ring: one set a dispatch, one dispatch a frame; reuse is the composition pass's assumption.
    CHECK(Vkr::kSets == 16);
}

int main()
{
    PlanCases();
    ViewCases();
    OutputCases();
    InterfaceCases();

    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }

    std::printf(
        "PASS: the Vulkan route's guide plan (matched, game, refused) against D3D12's arithmetic, the views the "
        "resample may read, its outputs, its push block and dispatch\n");
    return 0;
}
