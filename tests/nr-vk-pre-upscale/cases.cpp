#include <dlssnr/DlssNr_VkPreUpscale.h>
#include <dlssnr/DlssNr_VkPreObserve.h>
#include <cassert>
#include <iostream>
using namespace DlssNr::VkPre;
int main()
{
    Admission a;
    assert(Admit(a) == Refusal::Off);
    a.enabled = true;
    assert(Admit(a) == Refusal::Stage);
    a.stage = 1;
    assert(Admit(a) == Refusal::UnknownFeature);
    a.feature = Feature::SuperSampling;
    a.identity = a.mainPicture = a.observed = a.primary = a.oneTime = true;
    assert(Admit(a) == Refusal::None);
    a.feature = Feature::RayReconstruction;
    assert(Admit(a) == Refusal::RayReconstruction);
    a.feature = Feature::FrameGeneration;
    assert(Admit(a) == Refusal::FrameGeneration);
    a.feature = Feature::Unknown;
    assert(Admit(a) == Refusal::UnknownFeature);
    a.feature = Feature::SuperSampling;
    for (auto bridge :
         { Backend::DLSS_on12, Backend::XeSS_on12, Backend::FSR21_on12, Backend::FSR22_on12, Backend::FFX_on12 })
    {
        a.backend = bridge;
        assert(Admit(a) == Refusal::Bridge);
    }
    a.backend = Backend::RayReconstruction;
    assert(Admit(a) == Refusal::RayReconstruction);
    a.backend = Backend::Unknown;
    assert(Admit(a) == Refusal::Bridge);
    a.backend = Backend::Native;
    a.identity = false;
    assert(Admit(a) == Refusal::Identity);
    a.identity = true;
    a.mainPicture = false;
    assert(Admit(a) == Refusal::OtherPicture);
    a.mainPicture = true;
    a.observed = false;
    assert(Admit(a) == Refusal::Observation);
    a.observed = true;
    a.primary = false;
    assert(Admit(a) == Refusal::Recording);
    a.primary = true;
    a.oneTime = false;
    assert(Admit(a) == Refusal::Recording);
    a.oneTime = true;
    a.simultaneous = true;
    assert(Admit(a) == Refusal::Recording);
    a.simultaneous = false;
    a.outstanding = true;
    assert(Admit(a) == Refusal::Busy);
    a.outstanding = false;
    a.stage = 99;
    assert(Admit(a) == Refusal::Stage);
    assert(AllowPost(Outcome::Unselected));
    for (auto o : { Outcome::StandDown, Outcome::Rejected, Outcome::Partial, Outcome::Bound })
        assert(!AllowPost(o));
    Outstanding loan;
    Ticket t { 1, 2, 3 };
    assert(loan.Take(t));
    assert(!loan.Take(t));
    assert(!loan.Take({ 1, 9, 3 }));
    loan.Retire({ 1, 2, 2 });
    assert(loan.Busy());
    loan.Retire(t);
    assert(!loan.Busy());
    assert(!loan.Take({}));

    Observations r;
    r.Device(1);
    r.FeatureCreated(1, 7, Feature::SuperSampling);
    auto f = r.FindFeature(1, 7);
    assert(f && f->device == 1 && f->kind == Feature::SuperSampling);
    r.FeatureCreated(1, 7, Feature::RayReconstruction);
    assert(!r.Current(*f));
    f = r.FindFeature(1, 7);
    assert(f && f->kind == Feature::RayReconstruction);
    r.ForgetFeature(1, 7);
    assert(!r.FindFeature(1, 7));
    r.FeatureCreated(1, 7, Feature::SuperSampling);
    r.Pool(1, 10, 4);
    r.Allocate(1, 10, 20, true);
    r.Begin(20, true, true, false);
    auto c = r.Command(20);
    assert(c && c->recording && c->primary && c->oneTime && c->family == 4);
    assert(!c->ticket); // passive Begin without lifetime evidence cannot qualify a recording
    ImageFact im;
    im.device = 1;
    im.width = 100;
    im.height = 50;
    im.depth = 1;
    im.levels = 1;
    im.layers = 1;
    im.format = 37;
    im.usage = 12;
    im.samples = 1;
    im.type = 1;
    r.Image(30, im);
    Range range { 1, 0, 1, 0, 1 };
    r.View(40, { 1, 30, 37, 1, range });
    assert(r.ImageForView(1, 40));
    r.Barrier(20, 30, range, 0, 5, ~0u, ~0u);
    assert(r.Layout(20, 40) == 5);
    r.Barrier(20, 30, range, 1, 5, ~0u, ~0u); // inconsistent old layout is not evidence
    assert(!r.Layout(20, 40));
    r.Barrier(20, 30, range, 0, 5, ~0u, ~0u);
    r.Rendering(20, true, false);
    assert(r.Command(20)->inside);
    r.Rendering(20, false, false);
    assert(!r.Command(20)->inside);
    assert(r.Layout(20, 40) == 5);
    r.Rendering(20, true, true);
    r.Rendering(20, false, true);
    assert(!r.Layout(20, 40));
    r.Pipeline(20, 1, 99);
    uint64_t sets[] { 8, 9 };
    uint32_t offsets[] { 32 };
    r.Descriptors(20, 1, 70, 0, 2, sets, 1, offsets);
    uint32_t data = 0x12345678;
    r.Push(20, 70, 0x20, 0, 4, &data);
    data = 0;
    c = r.Command(20);
    assert(c->pipelines[1] == 99 && c->descriptors.size() == 1);
    assert(c->descriptors[0].sets[1] == 9 && c->descriptors[0].offsets[0] == 32);
    assert(c->push.size() == 1 && c->push[0].bytes[0] == 0x78);
    r.Unsupported(20);
    assert(!r.Command(20)->bindingsKnown);
    r.End(20, true);
    assert(!r.Command(20)->recording);
    r.Begin(20, false, true, false);
    assert(!r.Command(20)->recording); // failed begin
    r.Begin(20, true, true, false);
    assert(r.Command(20)->recording && !r.Command(20)->ticket && r.Command(20)->bindingsKnown);
    assert(r.Command(20)->descriptors.empty());
    r.ResetPool(1, 10, false);
    assert(r.Command(20)->recording);
    r.ResetPool(1, 10, true);
    assert(!r.Command(20)->recording);
    r.Begin(20, true, true, false);
    r.Barrier(20, 30, { 1, 0, 9, 0, 1 }, 0, 5, ~0u, ~0u);
    assert(!r.Layout(20, 40));
    r.Barrier(20, 30, range, 0, 5, 3, 4);
    assert(!r.Layout(20, 40));
    r.Barrier(20, 30, range, 0, 5, ~0u, ~0u);
    assert(r.Layout(20, 40) == 5);
    r.Rendering(20, true, false);
    assert(!r.Layout(20, 40));
    r.Rendering(20, false, false);
    r.ExecuteSecondary(20);
    assert(!r.Layout(20, 40) && !r.Command(20)->renderingKnown);
    r.Begin(20, true, true, false);
    r.Descriptors(20, 1, 70, 0, 33, sets, 0, nullptr);
    assert(!r.Command(20)->bindingsKnown);
    r.Begin(20, true, true, false);
    r.Push(20, 70, 0x20, 255, 4, &data);
    assert(!r.Command(20)->bindingsKnown);
    r.Begin(20, true, true, false);
    for (int n = 0; n < 65; ++n)
        r.Push(20, 70, 0x20, 0, 4, &data);
    assert(!r.Command(20)->bindingsKnown);
    r.DestroyImage(1, 30);
    assert(!r.ImageForView(1, 40));
    r.Image(30, im);
    assert(!r.ImageForView(1, 40)); // same handle, different incarnation
    r.View(40, { 1, 30, 37, 1, range });
    assert(r.ImageForView(1, 40));
    r.DestroyDevice(1);
    assert(!r.FindFeature(1, 7) && !r.Command(20) && !r.ImageForView(1, 40));
    r.Device(1);
    r.FeatureCreated(1, 7, Feature::SuperSampling);
    assert(!r.Current(*f));
    assert(!r.Command(999));
    std::cout << "PASS: admission, outcomes, single loan, observation and identity contracts\n";
}
