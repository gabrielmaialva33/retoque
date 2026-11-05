#include <dlssnr/DlssNr_VkPreObserve.h>
#include <cassert>
#include <iostream>
using namespace DlssNr::VkPre;
int main()
{
    Observations r;
    r.Device(1);
    r.Device(2);
    r.FeatureCreated(1, 7, Feature::SuperSampling, 107);
    auto first = r.FindFeature(1, 7);
    assert(first);
    r.FeatureCreated(2, 7, Feature::RayReconstruction, 207);
    assert(r.Current(*first));
    auto second = r.FindFeature(2, 7);
    assert(second && second->kind == Feature::RayReconstruction);
    r.ForgetFeatureSource(107, 7);
    assert(!r.FindFeature(1, 7) && r.Current(*second));
    r.FeatureCreated(1, 7, Feature::SuperSampling, 108);
    r.ForgetFeatureSource(107, 7);
    assert(r.FindFeature(1, 7)); // Late old pointer must not erase successor.
    r.ForgetFeature(1, 7);
    assert(r.Current(*second));

    ImageFact a;
    a.device = 1;
    a.width = 100;
    a.height = 50;
    a.depth = a.levels = a.layers = a.samples = 1;
    a.format = 37;
    a.usage = 12;
    a.type = 1;
    auto b = a;
    b.device = 2;
    b.width = 200;
    Range range { 1, 0, 1, 0, 1 };
    r.Image(30, a);
    r.Image(30, b);
    r.View(40, { 1, 30, 37, 1, range });
    r.View(40, { 2, 30, 37, 1, range });
    assert(r.ImageForView(1, 40)->width == 100 && r.ImageForView(2, 40)->width == 200);
    auto av = *r.ViewInfo(1, 40), bv = *r.ViewInfo(2, 40);
    r.View(40, { 1, 30, 37, 1, range }); // Same device, image, range and numeric view: new creation epoch.
    assert(!r.CurrentView(40, av) && r.CurrentView(40, bv));
    assert(r.ViewInfo(1, 40)->incarnation != av.incarnation);
    r.DestroyView(1, 40);
    assert(r.ImageForView(2, 40));
    r.View(40, { 1, 30, 37, 1, range });
    r.DestroyImage(1, 30);
    assert(!r.ImageForView(1, 40) && r.ImageForView(2, 40));
    r.Image(30, a);
    assert(!r.ImageForView(1, 40) && r.CurrentView(40, bv));
    r.View(40, { 1, 30, 37, 1, range });

    r.Pool(1, 10, 4);
    r.Pool(2, 10, 8);
    r.Allocate(1, 10, 20, true);
    r.Allocate(2, 10, 21, true);
    r.Begin(20, true, true, false);
    r.Begin(21, true, true, false);
    assert(r.Command(20)->family == 4 && r.Command(21)->family == 8);
    r.Barrier(20, 30, range, 0, 5, ~0u, ~0u);
    r.Barrier(21, 30, range, 0, 1, ~0u, ~0u);
    assert(r.Layout(20, 40) == 5 && r.Layout(21, 40) == 1);
    r.ResetPool(1, 10, true);
    assert(!r.Command(20)->recording && r.Command(21)->recording);
    r.Pool(1, 10, 9);
    assert(!r.Command(20) && r.Command(21)->recording);
    r.Allocate(1, 10, 20, true);
    r.DestroyPool(1, 10);
    assert(!r.Command(20) && r.Command(21)->family == 8);
    r.Pool(1, 10, 4);
    r.Allocate(1, 10, 21, true); // Dispatchable collision is refused, not picked by order.
    assert(!r.Command(21) && !r.DeviceOfCommand(21));
    assert(r.Command(1, 21) && r.Command(2, 21));
    r.Begin(21, true, true, false);
    assert(!r.Command(1, 21)->recording);
    r.Free(1, 21);
    assert(r.Command(21)->recording);
    r.Allocate(1, 10, 21, true);
    r.DestroyDevice(1);
    assert(r.Command(21)->device == 2 && r.CurrentView(40, bv) && r.Current(*second));
    r.Device(1);
    r.FeatureCreated(1, 7, Feature::SuperSampling, 107);
    assert(!r.Current(*first));
    r.DestroyDevice(2);
    assert(!r.Command(21) && !r.CurrentView(40, bv) && !r.Current(*second));
    std::cout << "PASS: composite identity, view epochs, exact release and cross-device teardown\n";
}
