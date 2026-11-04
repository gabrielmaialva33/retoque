// The main-picture rule (retoque/dlssnr/DlssNr_Pictures.h) and its glue (DlssNr_PictureGate.h) on fake parameters.
// No D3D12, no Vulkan, no NGX, no game.
#define main original_render_size_cases
#include "../nr-render-size/cases.cpp"
#undef main

#include <cstdio>
#include <string>
#include <vector>

static std::vector<std::string> g_logged;
template <class... Args> static void LogInfo(const char*, const Args&...) { g_logged.emplace_back("info"); }
#define LOG_INFO(...) LogInfo(__VA_ARGS__)

#include <dlssnr/DlssNr_RenderGeometry.h>
#include <dlssnr/DlssNr_PictureGate.h>

using namespace DlssNr::Pictures;

static unsigned g_checks = 0;
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                                          \
            std::abort();                                                                                              \
        }                                                                                                              \
        ++g_checks;                                                                                                    \
    } while (0)

constexpr uint64_t kWorld = 0x100000001ull, kPreview = 0x100000002ull, kWorld2 = 0x100000003ull;
constexpr uint64_t kWorldArea = 3440ull * 1440, kPreviewArea = 1024ull * 1024;
constexpr uintptr_t kWorldColour = 0x1000, kPreviewColour = 0x2000, kWorld2Colour = 0x3000;

static Call World() { return { kWorld, kWorldArea, kWorldColour }; }
static Call Preview() { return { kPreview, kPreviewArea, kPreviewColour }; }

static void OneFeature()
{
    Rule rule;
    auto first = rule.Judge(World());
    CHECK(first.role == Role::Main && first.took == Took::First && first.previous == 0 && !first.firstAside);
    for (int i = 0; i < 1000; ++i)
    {
        const auto judgement = rule.Judge(World());
        CHECK(judgement.role == Role::Main && judgement.took == Took::No);
    }
    // Made again on a resolution change, at another handle and size: main at once, without a history restart.
    CHECK(rule.Release(kWorld));
    const auto again = rule.Judge({ kWorld2, 1920ull * 1080, kWorld2Colour });
    CHECK(again.role == Role::Main && again.took == Took::Vacant && again.previous == 0);
    CHECK(!NewPicture(again.took) && !NewPicture(first.took) && again.epoch == 1 && first.epoch == 1);
    // Made again before the old one was released, the old one no longer evaluated: it hands over by quiet.
    Rule early;
    early.Judge(World());
    for (uint32_t i = 1; i < kQuietCalls; ++i)
        CHECK(early.Judge({ kWorld2, kWorldArea, kWorld2Colour }).role == Role::Aside);
    const auto quiet = early.Judge({ kWorld2, kWorldArea, kWorld2Colour });
    CHECK(quiet.role == Role::Main && quiet.took == Took::Quiet && quiet.previous == kWorld);
    CHECK(NewPicture(quiet.took) && quiet.epoch == 2 && early.Epoch() == 2);
}

static void PreviewNextToTheWorld()
{
    // The world first, the preview each frame after it: the preview is left as it is for as long as it is shown.
    Rule rule;
    rule.Judge(World());
    int firsts = 0;
    for (int frame = 0; frame < 500; ++frame)
    {
        CHECK(rule.Judge(World()).role == Role::Main);
        const auto preview = rule.Judge(Preview());
        CHECK(preview.role == Role::Aside && preview.took == Took::No);
        firsts += preview.firstAside;
    }
    CHECK(firsts == 1 && rule.Main() == kWorld && rule.Others() == 1);
    CHECK(rule.Judge(World()).epoch == 1 && rule.Judge(Preview()).epoch == 0);
    // The preview first in each frame: the same.
    Rule other;
    other.Judge(World());
    for (int frame = 0; frame < 500; ++frame)
    {
        CHECK(other.Judge(Preview()).role == Role::Aside);
        CHECK(other.Judge(World()).role == Role::Main);
    }
    // The preview's page closes: it is released and stops; the world goes on, and the preview made again is new.
    CHECK(!rule.Release(kPreview) && rule.Others() == 0);
    CHECK(rule.Judge(World()).role == Role::Main);
    CHECK(rule.Judge(Preview()).firstAside);
}

static void LargerPictureTakesMain()
{
    // The preview made first (a game that opens on its character page): the world, clearly larger, takes main.
    Rule rule;
    CHECK(rule.Judge(Preview()).took == Took::First);
    const auto world = rule.Judge(World());
    CHECK(world.role == Role::Main && world.took == Took::Larger && world.previous == kPreview);
    CHECK(NewPicture(world.took) && world.epoch == 2);
    // The old main is known as another picture from the takeover: the takeover's line said it, the aside line does not.
    CHECK(rule.Others() == 1);
    const auto old = rule.Judge(Preview());
    CHECK(old.role == Role::Aside && !old.firstAside);
    CHECK(rule.Judge(World()).role == Role::Main);
    // A quarter more area is clearly larger; less is not, and an unknown area never is.
    CHECK(ClearlyLarger(1251, 1000) && !ClearlyLarger(1250, 1000) && !ClearlyLarger(1000, 1000));
    CHECK(!ClearlyLarger(kWorldArea, 0));
    Rule unknown;
    unknown.Judge({ kPreview, 0, kPreviewColour });
    CHECK(unknown.Judge(World()).role == Role::Aside);
}

static void SameSizePictures()
{
    // Two pictures at one size and format, each on its own allocation: the second is left aside, never mixed in.
    Rule rule;
    rule.Judge(World());
    for (int frame = 0; frame < 200; ++frame)
    {
        CHECK(rule.Judge({ kPreview, kWorldArea, kPreviewColour }).role == Role::Aside);
        CHECK(rule.Judge(World()).role == Role::Main);
    }
}

static void ViewsOfMain()
{
    // One feature per screen plus the combined picture, all on one colour allocation: views of main, as before.
    Rule rule;
    rule.Judge({ kWorld, 5760ull * 1080, kWorldColour });
    for (int frame = 0; frame < 200; ++frame)
    {
        for (uint64_t screen = 0; screen < 3; ++screen)
            CHECK(rule.Judge({ 0x100000010ull + screen, 1920ull * 1080, kWorldColour }).role == Role::ViewOfMain);
        CHECK(rule.Judge({ kWorld, 5760ull * 1080, kWorldColour }).role == Role::Main);
    }
    // A screen first, the larger combined picture on the same allocation after it: a view, not a takeover.
    Rule screens;
    screens.Judge({ 0x100000010ull, 1920ull * 1080, kWorldColour });
    CHECK(screens.Judge({ kWorld, 5760ull * 1080, kWorldColour }).role == Role::ViewOfMain);
    // Views keep main alive: no quiet handover while only views of main call.
    for (uint32_t i = 0; i < 4 * kQuietCalls; ++i)
        CHECK(screens.Judge({ 0x100000011ull, 1920ull * 1080, kWorldColour }).role == Role::ViewOfMain);
    CHECK(screens.Main() == 0x100000010ull);
    // Main's own feature stops while its views go on: another picture calling between them is not let in as quiet.
    for (uint32_t i = 0; i < 4 * kQuietCalls; ++i)
    {
        CHECK(screens.Judge({ 0x100000011ull, 1920ull * 1080, kWorldColour }).role == Role::ViewOfMain);
        CHECK(screens.Judge({ kPreview, 1920ull * 1080, kPreviewColour }).role == Role::Aside);
    }
    CHECK(screens.Main() == 0x100000010ull);
    // An unknown colour is never a view.
    Rule unknown;
    unknown.Judge({ kWorld, kWorldArea, 0 });
    CHECK(unknown.Judge({ kPreview, kPreviewArea, 0 }).role == Role::Aside);
}

static void QuietMain()
{
    // A menu scene's feature the game never released stops; the world's calls take main after kQuietCalls.
    Rule rule;
    rule.Judge({ kPreview, kWorldArea, kPreviewColour });
    for (uint32_t i = 1; i < kQuietCalls; ++i)
        CHECK(rule.Judge(World()).role == Role::Aside);
    const auto taken = rule.Judge(World());
    CHECK(taken.took == Took::Quiet && taken.previous == kPreview && rule.Main() == kWorld);
    // Alternating with main never counts as quiet.
    Rule alive;
    alive.Judge(World());
    for (uint32_t i = 0; i < 10 * kQuietCalls; ++i)
    {
        CHECK(alive.Judge({ kPreview, kWorldArea, kPreviewColour }).role == Role::Aside);
        if (i % 4 == 0)
            alive.Judge(World());
    }
    CHECK(alive.Main() == kWorld);
}

static void MainReleasedWhilePreviewOpen()
{
    // The world made again on a resolution change while the preview is open: the preview, a known other picture,
    // waits; the world's new feature takes main as soon as it calls.
    Rule rule;
    rule.Judge(World());
    rule.Judge({ kPreview, kWorldArea, kPreviewColour });
    CHECK(rule.Release(kWorld) && rule.Main() == 0);
    CHECK(rule.Judge({ kPreview, kWorldArea, kPreviewColour }).role == Role::Aside);
    const auto successor = rule.Judge({ kWorld2, kWorldArea, kWorld2Colour });
    CHECK(successor.role == Role::Main && successor.took == Took::Vacant && !NewPicture(successor.took));
    CHECK(successor.epoch == 1);
    CHECK(rule.Judge({ kPreview, kWorldArea, kPreviewColour }).role == Role::Aside);
    // No successor comes: the preview takes main after kQuietCalls calls.
    Rule alone;
    alone.Judge(World());
    alone.Judge(Preview());
    alone.Release(kWorld);
    for (uint32_t i = 1; i < kQuietCalls; ++i)
        CHECK(alone.Judge(Preview()).role == Role::Aside);
    // The preview, another picture, takes main after the wait: a new picture, so a new epoch (the model is shown
    // another picture than the one its history holds).
    const auto waited = alone.Judge(Preview());
    CHECK(waited.took == Took::Waited && NewPicture(waited.took) && waited.epoch == 2);
    CHECK(alone.Main() == kPreview && alone.Others() == 0);
    // Same size and format: nothing else would restart the history (the review's F1 case).
    Rule same;
    same.Judge(World());
    same.Judge({ kPreview, kWorldArea, kPreviewColour });
    same.Release(kWorld);
    Judgement last;
    for (uint32_t i = 0; i < kQuietCalls; ++i)
        last = same.Judge({ kPreview, kWorldArea, kPreviewColour });
    CHECK(last.took == Took::Waited && last.epoch == 2);
    // Releases that are not main's change nothing; key 0 is no feature.
    Rule quiet;
    quiet.Judge(World());
    CHECK(!quiet.Release(0) && !quiet.Release(kPreview) && quiet.Main() == kWorld);
}

static void Epochs()
{
    // The holder of the history, under its own lock, in the order frames reach it.
    EpochGate gate;
    CHECK(gate.Consume(0) == Consumed::Same); // never judged: as before
    CHECK(gate.Consume(3) == Consumed::Same); // the first picture it sees is its own, whatever its number
    CHECK(gate.Consume(3) == Consumed::Same);
    CHECK(gate.Consume(0) == Consumed::Same);
    // The old picture's admitted frame arrives first, then the new picture's: only the new one restarts.
    CHECK(gate.Consume(4) == Consumed::Restart);
    CHECK(gate.Consume(4) == Consumed::Same);
    // The new picture's frame arrived first: a late frame of the old one is refused, never run on the new history.
    CHECK(gate.Consume(3) == Consumed::Stale);
    CHECK(gate.Consume(4) == Consumed::Same);
    // Two takeovers before the next frame: one restart.
    CHECK(gate.Consume(6) == Consumed::Restart && gate.Consume(5) == Consumed::Stale);
}

static void Glue()
{
    using namespace DlssNr::RenderGeometry;
    FakeParams creation, evaluate;
    creation.Put(NVSDK_NGX_Parameter_Width, 1720);
    creation.Put(NVSDK_NGX_Parameter_Height, 720);
    creation.Put(NVSDK_NGX_Parameter_OutWidth, 3440);
    creation.Put(NVSDK_NGX_Parameter_OutHeight, 1440);
    FakeParams previewCreation, previewEvaluate;
    previewCreation.Put(NVSDK_NGX_Parameter_Width, 512);
    previewCreation.Put(NVSDK_NGX_Parameter_Height, 512);
    previewCreation.Put(NVSDK_NGX_Parameter_OutWidth, 1024);
    previewCreation.Put(NVSDK_NGX_Parameter_OutHeight, 1024);
    const auto worldKey = Key(Api::D3D12, 7), previewKey = Key(Api::D3D12, 8);
    Remember(worldKey, ReadCreation(&creation));
    Remember(previewKey, ReadCreation(&previewCreation));

    // No binding (NR off at creation, a replay, another caller): main, the rule is not asked and nothing is read.
    CHECK(Ask(&evaluate, []() -> uintptr_t { std::abort(); }).role == Role::Main);
    CHECK(Ask(&evaluate, [] { return uintptr_t(kWorldColour); }).role == Role::Main && Instance().Main() == 0);
    CHECK(EvaluateEpoch(&evaluate) == 0);
    {
        // Bound but not judged yet: no epoch.
        Scope scope(true, worldKey, &evaluate);
        CHECK(EvaluateEpoch(&evaluate) == 0);
    }
    {
        Scope scope(true, worldKey, &evaluate);
        // Another parameter block than the bound one: main, unjudged.
        FakeParams foreign;
        CHECK(Ask(&foreign, [] { return uintptr_t(kWorldColour); }).role == Role::Main && Instance().Main() == 0);
        const auto first = Ask(&evaluate, [] { return uintptr_t(kWorldColour); });
        CHECK(first.role == Role::Main && first.took == Took::First && Instance().Main() == worldKey);
        // A second seam of the same evaluate gets the same answer without reading anything again.
        CHECK(Ask(&evaluate, []() -> uintptr_t { std::abort(); }).epoch == 1);
        const auto second = Ask(&evaluate, [] { return uintptr_t(0); });
        CHECK(second.role == Role::Main && second.took == Took::No && second.epoch == first.epoch);
        CHECK(EvaluateEpoch(&evaluate) == 1 && EvaluateEpoch(&foreign) == 0);
    }
    g_logged.clear();
    for (int frame = 0; frame < 3; ++frame)
    {
        {
            Scope scope(true, worldKey, &evaluate);
            CHECK(Ask(&evaluate, [] { return uintptr_t(kWorldColour); }).role == Role::Main);
        }
        {
            Scope scope(true, previewKey, &previewEvaluate);
            CHECK(Ask(&previewEvaluate, [] { return uintptr_t(kPreviewColour); }).role == Role::Aside);
            CHECK(Ask(&previewEvaluate, [] { return uintptr_t(kPreviewColour); }).role ==
                  Role::Aside); // the after-upscale seam, cached
            CHECK(EvaluateEpoch(&previewEvaluate) == 0);
        }
    }
    CHECK(g_logged.size() == 1); // the preview said once
    CHECK(g_asideEvaluates.load() == 3);
    // A disabled scope (NR off) binds nothing: main.
    {
        Scope scope(false, previewKey, &previewEvaluate);
        CHECK(Ask(&previewEvaluate, [] { return uintptr_t(kPreviewColour); }).role == Role::Main);
    }
    // The game releases the world: Forget tells the rule.
    Forget(worldKey);
    CHECK(Instance().Main() == 0);
    Forget(previewKey);
}

int main()
{
    OneFeature();
    PreviewNextToTheWorld();
    LargerPictureTakesMain();
    SameSizePictures();
    ViewsOfMain();
    QuietMain();
    MainReleasedWhilePreviewOpen();
    Epochs();
    Glue();
    std::printf("PASS: %u main-picture checks\n", g_checks);
    return 0;
}
