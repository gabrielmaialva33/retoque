#include "pch.h"

#include "DlssNr_Consumers.h"
#include "DlssNr_ConsumerMarkers.h"
#include <Config.h>
#include <Logger.h>
#include <filesystem>
#include <mutex>
#include <string>

namespace DlssNr::Consumers
{
namespace
{

struct Found
{
    const Competitor* competitor = nullptr;
    std::string module;
};

Found Scan()
{
    HMODULE modules[1024] {};
    DWORD needed = 0;

    // K32EnumProcessModules rather than the psapi import: kernel32 is already loaded and this keeps
    // the module list out of our own link-time dependencies.
    using PFN_EnumModules = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto enumModules = kernel32 != nullptr
                                 ? reinterpret_cast<PFN_EnumModules>(GetProcAddress(kernel32, "K32EnumProcessModules"))
                                 : nullptr;

    if (enumModules == nullptr || !enumModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
    {
        LOG_DEBUG("could not enumerate process modules; the competing-consumer check did not run");
        return {};
    }

    DWORD count = needed / sizeof(HMODULE);

    if (count > _countof(modules))
        count = _countof(modules);

    for (DWORD i = 0; i < count; ++i)
    {
        wchar_t buffer[MAX_PATH] {};

        if (GetModuleFileNameW(modules[i], buffer, MAX_PATH) == 0)
            continue;

        const std::filesystem::path module = buffer;

        if (const Competitor* competitor = MatchModuleName(module.filename().wstring()))
            return { competitor, module.filename().string() };
    }

    LOG_DEBUG("no competing DLSS 5 neural consumer found in this process");
    return {};
}

// Once per process. Producers are loaded by the time our model loader runs (DlssNr_Consumers.h), and
// one that arrives later is still caught by WarnIfCompeting's own scan at forwarder load.
const Found& Competing()
{
    static std::once_flag once;
    static Found found;
    std::call_once(once, [] { found = Scan(); });
    return found;
}

} // namespace

bool HandOverIfCompeting()
{
    const Found& found = Competing();

    if (found.competitor == nullptr || !Config::Instance()->DlssNrYieldToAddons.value_or_default())
        return false;

    static std::once_flag said;
    std::call_once(said,
                   [&found]
                   {
                       LOG_INFO("DLSS-NR: Neural Rendering is handed to {} (module {}) for this session: ours stays "
                                "off so the frame gets one neural pass, and that add-on runs over our upscaler's "
                                "output. Set [DlssNr] YieldToAddons=false to run ours beside it (two passes).",
                                found.competitor->project, found.module);
                   });

    // The toggle key's own mechanism: off for this session, the ini is not rewritten.
    Config::Instance()->DlssNrEnabled.set_volatile_value(false);
    return true;
}

void WarnIfCompeting()
{
    const Found& found = Competing();

    if (found.competitor == nullptr)
        return;

    static std::once_flag once;
    std::call_once(once,
                   [&found]
                   {
                       LOG_ERROR("another DLSS 5 neural consumer is in this process: {} (module {}). It reaches "
                                 "the model by detouring NVIDIA's core, and we ARE the module the process talks "
                                 "to, so our upscaler's call into the core fires its detour as well -- the frame "
                                 "gets TWO neural passes. Remove one of them: either that add-on, or set [DlssNr] "
                                 "Enabled=false here (YieldToAddons=true does that for the session).",
                                 found.competitor->project, found.module);
                   });
}

} // namespace DlssNr::Consumers
