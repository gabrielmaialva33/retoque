#include "pch.h"
#include <dlssnr/DlssNr_AdvisorRuntime.h>
// Reuse the existing synthetic device/queue/recording fixture; compile the REAL collector in a separate TU.
#include "timing_fixture.h"

int main()
{
    Advisor::Context context;
    context.device = 1;
    assert(AdvisorRuntime::Start({ 1, 1 }, 60));
    const auto decision = AdvisorRuntime::Frame(context, 1, true);
    Device device;
    List list;
    list.device = &device;
    GpuTiming::Metadata metadata;
    metadata.advisorToken = decision.token;
    metadata.advisorFrameMs = 16;
    metadata.contractHash = 123;
    {
        GpuTiming::Evaluation timing(&device, &list, metadata, true, 1);
        timing.ModelBegin();
        timing.ModelEnd();
        timing.FinishOnScopeExit(1, true);
    }
    GpuTiming::Poll();
    assert(maps == 0 && AdvisorRuntime::Snapshot().Count() == 0);
    Execute(list);
    Seal(list);
    GpuTiming::Poll();
    assert(maps == 0 && AdvisorRuntime::Snapshot().Count() == 0);
    completed[0] = 10;
    GpuTiming::Poll();
    assert(maps == 1 && AdvisorRuntime::Snapshot().Count() == 1);
    GpuTiming::Poll();
    assert(maps == 1 && AdvisorRuntime::Snapshot().Count() == 1);
    AdvisorRuntime::Cancel(Advisor::Abort::Cancelled);
    assert(AdvisorRuntime::Start({ 1, 1 }, 60));
    AdvisorRuntime::Frame(context, 2, true);
    // A completed old recording must not become a sample in a new run.
    List old;
    old.device = &device;
    {
        GpuTiming::Evaluation timing(&device, &old, metadata, true, 1);
        timing.ModelBegin();
        timing.ModelEnd();
        timing.FinishOnScopeExit(1, true);
    }
    Execute(old);
    Seal(old);
    GpuTiming::Poll();
    assert(maps == 2 && AdvisorRuntime::Snapshot().Count() == 0);
    AdvisorRuntime::Cancel(Advisor::Abort::Cancelled);
    GpuTiming::SetEnabled(false);
    assert(device.references == 1);
}
