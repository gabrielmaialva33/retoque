#include "pch.h"
#include "DlssNr_CrashNote.h"

#include <Config.h>

#include <atomic>
#include <cstdint>

// The Win32 half of the crash note (DlssNr_CrashNote.h, docs/dlssnr/design/engagement-funnel.md).
//
// A last-chance filter only, never a vectored handler: the filter runs once, for the exception that is about to end the
// process, while a vectored handler runs for every first-chance exception the game raises and handles (C++ throws,
// guard pages, probes), whose outcome is not ours to judge. Upstream installs neither; this is the only one in the
// tree.
//
// Installed lazily, at the first evaluate with NR on: with NR off nothing is installed, and an engine that sets its own
// filter at start-up (most do, before any evaluate) is chained to rather than replaced. An engine that sets its filter
// after ours, without chaining, takes the slot, and then nothing is written; that is the known gap.
//
// The filter formats into its own stack, writes with WriteFile on a handle opened at install, and hands the exception
// to the filter it replaced, or continues the search when there was none. It never handles the crash.

namespace
{
std::atomic<bool> g_tried { false };
std::atomic<bool> g_entered { false };
HANDLE g_file = INVALID_HANDLE_VALUE;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
const unsigned char* g_executable = nullptr; // the game's executable's base, taken at install
bool g_installed = false;

// The image holding address: its base from VirtualQuery (a system call, no loader lock), its name from its own export
// directory. GetModuleHandle, GetModuleHandleEx and GetModuleFileName may take the loader lock (Wine's do), which the
// dying thread or another may hold.
void Locate(uint64_t address, DlssNr::CrashNote::Fault& fault, char* name, size_t cap)
{
    MEMORY_BASIC_INFORMATION region {};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) == 0 ||
        region.Type != MEM_IMAGE || region.AllocationBase == nullptr)
    {
        fault.image = DlssNr::CrashNote::Image::None;
        return;
    }

    const auto base = static_cast<const unsigned char*>(region.AllocationBase);
    fault.offset = address - reinterpret_cast<uint64_t>(base);
    if (base == g_executable)
    {
        fault.image = DlssNr::CrashNote::Image::Executable;
        return;
    }

    MEMORY_BASIC_INFORMATION headers {};
    size_t headerBytes = 0;
    if (VirtualQuery(base, &headers, sizeof(headers)) != 0 && headers.State == MEM_COMMIT)
        headerBytes = headers.RegionSize;
    // The loader mapped SizeOfImage; WriteNote's __try catches an image that lies about it.
    if (DlssNr::CrashNote::ImageExportName(base, headerBytes, SIZE_MAX, name, cap))
    {
        fault.image = DlssNr::CrashNote::Image::Named;
        fault.moduleName = name;
    }
    else
        fault.image = DlssNr::CrashNote::Image::Unnamed;
}

void WriteNote(EXCEPTION_POINTERS* info)
{
    __try
    {
        if (g_file == INVALID_HANDLE_VALUE || info == nullptr || info->ExceptionRecord == nullptr)
            return;

        const EXCEPTION_RECORD& record = *info->ExceptionRecord;
        DWORD written = 0;
        if (record.ExceptionCode == 0xC00000FD)
        {
            WriteFile(g_file, DlssNr::CrashNote::kStackOverflowLine,
                      static_cast<DWORD>(sizeof(DlssNr::CrashNote::kStackOverflowLine) - 1), &written, nullptr);
            return;
        }

        DlssNr::CrashNote::Fault fault;
        fault.code = record.ExceptionCode;
        fault.address = reinterpret_cast<uint64_t>(record.ExceptionAddress);
        fault.thread = GetCurrentThreadId();
        if ((record.ExceptionCode == 0xC0000005 || record.ExceptionCode == 0xC0000006) && record.NumberParameters >= 2)
        {
            fault.accessKind = static_cast<int>(record.ExceptionInformation[0]);
            fault.accessTarget = static_cast<uint64_t>(record.ExceptionInformation[1]);
        }
        char name[64];
        Locate(fault.address, fault, name, sizeof(name));

        SYSTEMTIME now {};
        GetLocalTime(&now);
        const DlssNr::CrashNote::Clock clock { now.wHour, now.wMinute, now.wSecond, now.wMilliseconds };

        char note[DlssNr::CrashNote::kNoteBytes];
        const auto route = DlssNr::Funnel::Route::D3d12AfterUpscale;
        const size_t length = DlssNr::CrashNote::Format(note, sizeof(note), fault, clock, DlssNr::Funnel::LastStage(),
                                                        DlssNr::Funnel::Read(route), route);
        WriteFile(g_file, note, static_cast<DWORD>(length), &written, nullptr);
        FlushFileBuffers(g_file);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // A fault while writing the note ends the note, not the chain: the filter still hands the crash on.
    }
}

LONG WINAPI Filter(EXCEPTION_POINTERS* info)
{
    // Entered again: a fault in the note, or a chained filter that calls back into this one. Never chain from here.
    if (g_entered.exchange(true))
        return EXCEPTION_CONTINUE_SEARCH;

    WriteNote(info);

    // The crash is handled exactly as it would have been without the note.
    return g_previous != nullptr ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
}

void Install()
{
    const auto* config = Config::Instance();
    if (!config->LogToFile.value_or_default())
        return;

    // A second handle on the log spdlog already writes: appended at the end whatever spdlog's position, shared with
    // every access so neither the logger, a reader nor the log's rotation (PrepareLogger renames it) is refused.
    const std::wstring path = config->LogFileName.value_or_default();
    g_file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE)
    {
        LOG_INFO("DLSS-NR crash note: OptiScaler's log could not be opened for appending; no crash note this session");
        return;
    }

    g_executable = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    g_previous = SetUnhandledExceptionFilter(Filter);
    g_installed = true;
    LOG_INFO("DLSS-NR crash note: in place ({} filter before it, chained to)", g_previous != nullptr ? "a" : "no");
}
} // namespace

namespace DlssNr::CrashNote
{
void InstallOnce()
{
    if (g_tried.load(std::memory_order_relaxed))
        return;
    if (g_tried.exchange(true))
        return;
    Install();
}

void Uninstall()
{
    if (!g_installed)
        return;
    g_installed = false;
    const auto current = SetUnhandledExceptionFilter(g_previous);
    if (current != Filter)
        SetUnhandledExceptionFilter(current);
    // The handle stays open: another thread may be inside the filter as the library goes, and a handle is all it costs.
}
} // namespace DlssNr::CrashNote
