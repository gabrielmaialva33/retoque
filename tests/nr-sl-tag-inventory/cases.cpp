// Which buffers the game tags for Streamline (retoque/dlssnr/DlssNr_SlTagInventory.h): what the hooks note, what the
// line says and when, and what a HUD-aware present could expect of the HUD-less colour and the UI alpha. No Streamline,
// no D3D12, no game: the tags are the shape slSetTag hands the hooks, written out as numbers.
#include "retoque/dlssnr/DlssNr_SlTagInventory.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace DlssNr::SlTagInventory;

namespace
{
// sl::Resource and sl::ResourceTag as far as NoteTags reads them.
struct FakeResource
{
    void* native = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t nativeFormat = 0;
};
struct FakeExtent
{
    uint32_t top = 0;
    uint32_t left = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};
struct FakeTag
{
    FakeResource* resource = nullptr;
    uint32_t type = 0;
    uint32_t lifecycle = 0;
    FakeExtent extent {};
};

int g_texture = 0;

const auto kNoDescribe = [](const FakeResource&, Shape&) {};

bool Contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

void NothingUnarmed()
{
    Inventory inv;
    FakeResource r { &g_texture, 3440, 1440, 10 };
    FakeTag tag { &r, kHudLessColor, kValidUntilPresent };
    NoteTags(inv, &tag, 1, kViaSetTag, kNoDescribe);
    inv.Note(kDepth, true, kValidUntilPresent, kViaSetTag, Shape { 1, 1, 1 });
    assert(inv.Tags(kHudLessColor) == 0 && inv.Tags(kDepth) == 0);
    std::string line;
    assert(!inv.TakeLine(line, 10000) && line.empty());
    assert(inv.HudLess() == AtPresent::NotTagged && inv.UiAlpha() == AtPresent::NotTagged);
}

void FirstLineWaitsForATag()
{
    Inventory inv;
    inv.Arm();
    std::string line;
    assert(!inv.TakeLine(line, 5000));
    // A removal (no resource) is counted but is not a buffer the game provides.
    FakeTag removal { nullptr, kHudLessColor, kValidUntilPresent };
    NoteTags(inv, &removal, 1, kViaSetTag, kNoDescribe);
    FakeResource empty {};
    FakeTag nullNative { &empty, kUiColorAndAlpha, kValidUntilPresent };
    NoteTags(inv, &nullNative, 1, kViaSetTag, kNoDescribe);
    assert(inv.Removals() == 2 && inv.Tags(kHudLessColor) == 0 && inv.Tags(kUiColorAndAlpha) == 0);
    assert(!inv.TakeLine(line, 6000));
}

void SaysWhatWasTagged()
{
    Inventory inv;
    inv.Arm();
    FakeResource colour { &g_texture, 3440, 1440, 10 };
    FakeResource depth { &g_texture, 1720, 720, 40 };
    FakeTag tags[] = {
        { &colour, kHudLessColor, kValidUntilPresent },
        { &depth, kDepth, kOnlyValidNow },
        { &colour, kUiColorAndAlpha, kValidUntilEvaluate },
    };
    NoteTags(inv, tags, 3, kViaSetTagForFrame, kNoDescribe);
    std::string line;
    assert(inv.TakeLine(line, 1000));
    assert(Contains(line, "HUD-less colour (until Present, per frame, 3440x1440, format 10)"));
    assert(Contains(line, "depth (only valid now, per frame, 1720x720, format 40)"));
    assert(Contains(line, "UI colour+alpha (until evaluate, per frame, 3440x1440, format 10)"));
    assert(Contains(line, "HUD-less colour valid until Present"));
    assert(Contains(line, "UI colour+alpha not valid at Present"));
    assert(Contains(line, "(log only)"));
    assert(inv.HudLess() == AtPresent::Valid && inv.UiAlpha() == AtPresent::NotValid);
    // Depth is listed before the HUD-less colour: types in Streamline's order.
    assert(line.find("depth (") < line.find("HUD-less colour ("));
}

void SaidOnceWhileNothingChanges()
{
    Inventory inv;
    inv.Arm();
    FakeResource colour { &g_texture, 1920, 1080, 28 };
    FakeTag tag { &colour, kHudLessColor, kValidUntilPresent };
    NoteTags(inv, &tag, 1, kViaSetTag, kNoDescribe);
    std::string line;
    assert(inv.TakeLine(line, 1000));
    for (uint64_t t = 1000; t < 60000; t += 500)
    {
        NoteTags(inv, &tag, 1, kViaSetTag, kNoDescribe);
        std::string again;
        assert(!inv.TakeLine(again, t + kMinGapMs));
    }
    assert(inv.Tags(kHudLessColor) > 100);
}

void ChangeSaidAfterTheGap()
{
    Inventory inv;
    inv.Arm();
    FakeResource colour { &g_texture, 1920, 1080, 28 };
    FakeTag tag { &colour, kHudLessColor, kValidUntilPresent };
    NoteTags(inv, &tag, 1, kViaSetTag, kNoDescribe);
    std::string line;
    assert(inv.TakeLine(line, 10000));
    // A resize: a new shape is a change, said only once the gap has passed.
    colour.width = 2560;
    colour.height = 1440;
    NoteTags(inv, &tag, 1, kViaSetTag, kNoDescribe);
    assert(!inv.TakeLine(line, 10000 + kMinGapMs - 1));
    assert(inv.TakeLine(line, 10000 + kMinGapMs));
    assert(Contains(line, "2560x1440"));
    // A second lifecycle for the same type is a change and makes the verdict mixed.
    FakeTag now { &colour, kHudLessColor, kOnlyValidNow };
    NoteTags(inv, &now, 1, kViaSetTag, kNoDescribe);
    assert(inv.TakeLine(line, 20000));
    assert(Contains(line, "HUD-less colour (only valid now/until Present"));
    assert(Contains(line, "HUD-less colour valid until Present only sometimes"));
    assert(inv.HudLess() == AtPresent::Mixed);
}

void DescribeFillsTheShape()
{
    Inventory inv;
    inv.Arm();
    FakeResource bare { &g_texture };
    FakeTag tag { &bare, kMotionVectors, kValidUntilPresent };
    int described = 0;
    NoteTags(inv, &tag, 1, kViaSetTag,
             [&described](const FakeResource& resource, Shape& s)
             {
                 assert(resource.native == &g_texture);
                 ++described;
                 s.width = 1280;
                 s.height = 720;
                 s.format = 34;
             });
    assert(described == 1);
    const Shape s = inv.ShapeOf(kMotionVectors);
    assert(s.width == 1280 && s.height == 720 && s.format == 34 && !s.subrect);
    // No resource: nothing to describe.
    FakeTag removal { nullptr, kMotionVectors, kValidUntilPresent };
    NoteTags(inv, &removal, 1, kViaSetTag, [&described](const FakeResource&, Shape&) { ++described; });
    assert(described == 1);
}

void SubrectAndExtentFallback()
{
    Inventory inv;
    inv.Arm();
    FakeResource big { &g_texture, 3840, 2160, 10 };
    FakeTag part { &big, kScalingOutputColor, kValidUntilPresent, FakeExtent { 0, 0, 3440, 1440 } };
    NoteTags(inv, &part, 1, kViaSetTag, kNoDescribe);
    assert(inv.ShapeOf(kScalingOutputColor).subrect);
    FakeTag offset { &big, kScalingInputColor, kValidUntilPresent, FakeExtent { 8, 0, 3840, 2152 } };
    NoteTags(inv, &offset, 1, kViaSetTag, kNoDescribe);
    assert(inv.ShapeOf(kScalingInputColor).subrect);
    FakeTag whole { &big, kBackbuffer, kValidUntilPresent, FakeExtent { 0, 0, 3840, 2160 } };
    NoteTags(inv, &whole, 1, kViaSetTag, kNoDescribe);
    assert(!inv.ShapeOf(kBackbuffer).subrect);
    // A resource that says nothing of its size: the tag's extent stands in.
    FakeResource unsized { &g_texture };
    FakeTag sized { &unsized, kExposure, kValidUntilPresent, FakeExtent { 0, 0, 1, 1 } };
    NoteTags(inv, &sized, 1, kViaSetTag, kNoDescribe);
    const Shape s = inv.ShapeOf(kExposure);
    assert(s.width == 1 && s.height == 1 && !s.subrect);
    std::string line;
    assert(inv.TakeLine(line, 1));
    assert(Contains(line, "scaling output (until Present, slSetTag, 3840x2160, format 10, subrect)"));
}

void ViaIsKeptPerType()
{
    Inventory inv;
    inv.Arm();
    FakeResource r { &g_texture, 8, 8, 2 };
    FakeTag tag { &r, kDepth, kValidUntilPresent };
    NoteTags(inv, &tag, 1, kViaSetTag, kNoDescribe);
    NoteTags(inv, &tag, 1, kViaEvaluate, kNoDescribe);
    assert(inv.Via(kDepth) == (kViaSetTag | kViaEvaluate));
    std::string line;
    assert(inv.TakeLine(line, 1));
    assert(Contains(line, "depth (until Present, slSetTag, evaluate input, 8x8, format 2)"));
}

void UnknownTypesAndLifecycles()
{
    Inventory inv;
    inv.Arm();
    FakeResource r { &g_texture, 4, 4, 1 };
    FakeTag unnamed { &r, 70, kValidUntilPresent };
    FakeTag past { &r, kTypes, kValidUntilPresent };
    FakeTag odd { &r, kNoWarpMask, 7 };
    NoteTags(inv, &unnamed, 1, kViaSetTag, kNoDescribe);
    NoteTags(inv, &past, 1, kViaSetTag, kNoDescribe);
    NoteTags(inv, &odd, 1, kViaSetTag, kNoDescribe);
    assert(inv.Tags(70) == 1 && inv.Tags(kTypes) == 1);
    std::string line;
    assert(inv.TakeLine(line, 1));
    assert(Contains(line, "type 70 (until Present"));
    assert(Contains(line, "no-warp mask (unknown lifecycle"));
    assert(Contains(line, "types past 95"));
    // An unknown lifecycle is not "valid until Present".
    assert(PresentValidity(1u << 3) == AtPresent::NotValid);
}

void BufferIsNotAnImage()
{
    Inventory inv;
    inv.Arm();
    // A buffer whose byte width would wrap a 16-bit field to 4 or to 0: said as a buffer, with no size.
    FakeResource buffer { &g_texture, 0, 0, 0 };
    FakeTag tag { &buffer, kExposure, kValidUntilPresent, FakeExtent { 0, 0, 65540, 1 } };
    const auto asBuffer = [](const FakeResource&, Shape& s)
    {
        s = {};
        s.buffer = true;
    };
    NoteTags(inv, &tag, 1, kViaSetTag, asBuffer);
    const Shape s = inv.ShapeOf(kExposure);
    assert(s.buffer && s.width == 0 && s.height == 0 && s.format == 0 && !s.subrect);
    std::string line;
    assert(inv.TakeLine(line, 1));
    assert(Contains(line, "exposure (until Present, slSetTag, buffer)"));
    assert(!Contains(line, "65540") && !Contains(line, "4x1"));
}

void PackRoundTrip()
{
    const Shape s { 7680, 4320, 115, true };
    const Shape back = Unpack(Pack(s));
    assert(back.width == 7680 && back.height == 4320 && back.format == 115 && back.subrect);
    // A Vulkan extension format number does not spill into the other fields.
    const Shape big { 16, 16, 1000156000u, false };
    const Shape clamped = Unpack(Pack(big));
    assert(clamped.width == 16 && clamped.height == 16 && clamped.format == 0x7FFF && !clamped.subrect);
    assert(Pack(Shape {}) != 0); // a stored all-zero shape is still a stored shape
    Shape buffer;
    buffer.buffer = true;
    const Shape b = Unpack(Pack(buffer));
    assert(b.buffer && !b.subrect && b.width == 0 && b.format == 0);
}

void ManyThreadsNote()
{
    Inventory inv;
    inv.Arm();
    FakeResource r { &g_texture, 64, 64, 10 };
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back(
            [&inv, &r, t]
            {
                FakeTag tag { &r, static_cast<uint32_t>(t), kValidUntilPresent };
                for (int i = 0; i < 10000; ++i)
                    NoteTags(inv, &tag, 1, kViaSetTagForFrame, kNoDescribe);
            });
    }
    std::string line;
    for (uint64_t now = 1; now < 200000; now += kMinGapMs)
        inv.TakeLine(line, now);
    for (auto& thread : threads)
        thread.join();
    for (uint32_t t = 0; t < 4; ++t)
        assert(inv.Tags(t) == 10000);
}
} // namespace

int main()
{
    NothingUnarmed();
    FirstLineWaitsForATag();
    SaysWhatWasTagged();
    SaidOnceWhileNothingChanges();
    ChangeSaidAfterTheGap();
    DescribeFillsTheShape();
    SubrectAndExtentFallback();
    ViaIsKeptPerType();
    UnknownTypesAndLifecycles();
    BufferIsNotAnImage();
    PackRoundTrip();
    ManyThreadsNote();
    std::puts("nr-sl-tag-inventory: 12 cases passed");
    return 0;
}
