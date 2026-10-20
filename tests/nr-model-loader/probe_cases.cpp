#include "probe_fakes.h"

namespace DlssNr::Dx12Internal
{
NrState g_nr;
std::mutex g_nrMutex;
std::filesystem::path g_dllDir = "fixture";
bool EnsureForwarder()
{
    ++calls.ensures;
    // The caller must hold the same mutex. A second thread can safely test its ownership.
    bool acquired = true;
    std::thread observer(
        [&]
        {
            acquired = g_nrMutex.try_lock();
            if (acquired)
                g_nrMutex.unlock();
        });
    observer.join();
    assert(!acquired);
    g_nr.forwarder = calls.direct ? nullptr : reinterpret_cast<void*>(1);
    return !calls.ensureFails;
}
const char* NgxResultName(unsigned int) { return "fixture"; }
} // namespace DlssNr::Dx12Internal

static int Probe(const wchar_t*)
{
    ++calls.probes;
    return calls.incomplete ? 7 : 15;
}
static int Requirements(const wchar_t*, void* adapter, unsigned* supported, unsigned* arch, unsigned* os)
{
    ++calls.requirements;
    assert(adapter == (calls.queryFails || calls.adapterFails ? &fallbackAdapter : &gameAdapter));
    *supported = 0;
    *arch = *os = 1;
    return 1;
}
static int Init(const wchar_t*, const wchar_t*, void* device, int appId, int* attempt, int* results)
{
    ++calls.inits;
    assert(device != nullptr && appId == 0x15);
    *attempt = 1;
    results[0] = 1;
    return 1;
}
void* GetProcAddress(void* module, const char* name)
{
    assert(module != nullptr);
    ++calls.symbols;
    if (calls.missingExports)
        return nullptr;
    if (std::string_view(name) == "dlssnr_d3d11_probe")
        return reinterpret_cast<void*>(&Probe);
    if (std::string_view(name) == "dlssnr_d3d11_init")
        return reinterpret_cast<void*>(&Init);
    assert(std::string_view(name) == "dlssnr_d3d11_requirements");
    return reinterpret_cast<void*>(&Requirements);
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    const std::string_view mode = argv[1];
    IUnknown device;
    // Neither null nor default-off consumes the once flag or touches the loader/COM/model.
    DlssNr::ProbeD3D11(nullptr);
    DlssNr::ProbeD3D11(&device);
    assert(calls.ensures == 0 && calls.symbols == 0 && calls.queried == 0);
    Config::Instance()->DlssNrProbeD3D11.value = true;
    calls.direct = mode == "direct";
    calls.ensureFails = mode == "ensure-fails";
    calls.missingExports = mode == "no-exports";
    calls.missingSnippet = mode == "no-snippet";
    calls.queryFails = mode == "query-fails";
    calls.adapterFails = mode == "adapter-fails";
    calls.incomplete = mode == "incomplete";
    DlssNr::ProbeD3D11(&device);
    assert(calls.ensures == 1);
    const bool early = calls.direct || calls.ensureFails || calls.missingExports || calls.missingSnippet;
    if (calls.direct || calls.ensureFails)
        assert(calls.symbols == 0);
    assert(calls.probes == (early ? 0 : 1));
    assert(calls.requirements == (early ? 0 : 1));
    assert(calls.inits == (early || calls.incomplete ? 0 : 1));
    assert(calls.dxgiReleased == (!early && !calls.queryFails ? 1 : 0));
    const bool fallback = !early && (calls.queryFails || calls.adapterFails);
    assert(calls.gameReleased == (!early && !fallback ? 1 : 0));
    assert(calls.fallbackReleased == (fallback ? 1 : 0));
    assert(calls.factoryReleased == (fallback ? 1 : 0));
    const auto symbols = calls.symbols;
    DlssNr::ProbeD3D11(&device);
    assert(calls.ensures == 1 && calls.symbols == symbols); // failed/direct probes also stay one-shot
}
