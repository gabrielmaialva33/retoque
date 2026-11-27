#include "pch.h"
int main()
{
    CheckStatusCollector();
    MenuWindowOwnerPolicy::Owner<int> owner;
    assert(!owner.Accept(0, false, 0));
    assert(owner.Accept(10, false, 0));
    for (uint64_t now = 16; now < 6000; now += 16)
    {
        assert(owner.Accept(10, true, now));
        assert(!owner.Accept(20, true, now + 8));
        assert(owner.Handle() == 10);
    }
    // Secondary presents do not keep the silent owner alive. Exact timeout is inclusive.
    assert(owner.Accept(10, true, 6000));
    assert(!owner.Accept(20, true, 6999));
    assert(owner.Accept(20, true, 7000));
    assert(!owner.Accept(10, true, 7001));
    // Destruction bypasses the timeout; null presents never claim or refresh it.
    assert(owner.Accept(30, false, 7002));
    assert(!owner.Accept(0, true, 8001));
    assert(owner.Accept(40, true, 8002));
    assert(!owner.Accept(50, true, 7000)); // defensive clock rollback: do not underflow the age

    // Real production WndProc body: foreign/lost chain survives until its last message,
    // which is forwarded exactly once even when the ordinary handler consumes messages.
    _state.OriginalWndProcs[10] = Original;
    _state.OriginalWndProcs[20] = Original;
    assert(OptiInputWndProc(10, WM_NULL, 0, 0) == 43);
    consume = true;
    const int before = handledCalls;
    assert(OptiInputWndProc(10, WM_NCDESTROY, 0, 0) == 42);
    assert(handledCalls == before);
    assert(!_state.OriginalWndProcs.contains(10));
    assert(_state.OriginalWndProcs.contains(20));
    assert(originalCalls == 2);
    assert(OptiInputWndProc(10, WM_NULL, 0, 0) == 17); // reused HWND cannot inherit previous original
    assert(OptiInputWndProc(99, WM_NCDESTROY, 0, 0) == 17);
    assert(defaultCalls == 2);
    _state.OriginalWndProcs[10] = Original; // a fresh installation on the reused value
    assert(OptiInputWndProc(10, WM_NCDESTROY, 0, 0) == 42);
    assert(!_state.OriginalWndProcs.contains(10));

    using namespace DlssNr::StatusPage;
    Inputs in;
    in.backend = Backend::D3D12;
    in.failure = "stale failure";
    assert(Rows(in).size() == 1 && StateText(in) == "Off");
    in.enabled = true;
    assert(StateText(in) == "Failed / unavailable: stale failure");
    in.failure.clear();
    in.backend = Backend::None;
    assert(StateText(in) == "On, waiting for the model");
    in.backend = Backend::Vulkan;
    assert(StateText(in) == "On (native Vulkan; model loaded)");
    in.apply = false;
    assert(StateText(in).find("edit hidden") != std::string::npos);
    in.backend = Backend::D3D12;
    assert(StateText(in).find("D3D12") != std::string::npos);
    in.passes = 1;
    auto rows = Rows(in);
    assert(rows[2].value == "1 pass");
    in.passes = 3;
    in.cadence = 4;
    in.route = "native D3D11 DLSS cannot feed this pass";
    in.hook = "present host";
    in.cadenceStatus = "Unavailable on this route";
    rows = Rows(in);
    assert(rows[1].value == in.hook && rows[2].value == in.route);
    assert(rows[3].value == "3 passes");
    assert(rows[6].value == "One model frame in 4");
    assert(rows[7].value == in.cadenceStatus);
    in.backend = Backend::Vulkan;
    assert(Rows(in).size() == 7); // do not report stale D3D12 cadence state for Vulkan
    assert(ScaleText(0.5f, 0.75f) == "50%; after RR 75% (resolved settings)");
    assert(ModelText("") == "Auto (nvngx_dlssnr.dll search)");
    assert(ModelText("C:\\Users\\Example\\model.dll") == "model.dll (startup selection)");
    assert(ModelText("/home/example/model.dll") == "model.dll (startup selection)");
    assert(ModelText("my-model.dll") == "my-model.dll (startup selection)");
    std::cout << "PASS: owner sequences, production WndProc, NR status matrix\n";
}
