#pragma once

// The NR crash note (docs/dlssnr/design/engagement-funnel.md, "The crash note").
//
// When a game with NR on dies of an unhandled exception, OptiScaler's log ends at whatever was last logged, often
// minutes earlier, and says nothing of the death: not the code, not the module, not whether NR was in the middle of
// its pass. A last-chance filter (DlssNr_CrashNote.cpp) writes three lines to that log as the process dies:
//
//   [hh:mm:ss.ffffff] [E] DLSS-NR crash note: unhandled exception 0xC0000005 (access violation, write at 0x...)
//                         at nvngx_dlssnr.dll+0x1a2b3c, thread 1234
//   [hh:mm:ss.ffffff] [E] DLSS-NR crash note: last NR stage: D3D12 after the upscaler, recording the pass, on this
//                         thread
//   [hh:mm:ss.ffffff] [E] DLSS-NR crash note: funnel d3d12_after_upscale eligible=... ran=... model=... declined=...
//                         unaccounted=... top=<tag>:<count>
//
// after RenoDX-DLSS5-Generic's lastgasp.hpp (PEQHUB, MIT). The filter runs on a dying process whose heap or locks may
// be what broke, so this half is formatting only: by hand, into the caller's buffer, with no allocation, no printf, no
// locale and no clock. The module is named by its PE export name, read from the mapped image (no loader lock, no
// path): the text never carries a path or a user name.
//
// Standard library and the funnel only, so tests/nr-crash-note compiles it as it is, with every allocation counted.

#include "DlssNr_Funnel.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace DlssNr::CrashNote
{
// The note's buffer: three lines with room to spare. On the filter's stack.
inline constexpr size_t kNoteBytes = 768;

// A stack overflow leaves the filter only the pages the guard gave back: this line is written as it is, nothing is
// formatted.
inline constexpr char kStackOverflowLine[] =
    "[E] DLSS-NR crash note: unhandled stack overflow (0xC00000FD); nothing more is formatted on an exhausted stack\n";

enum class Image : uint8_t
{
    None,       // the address is in no mapped image: JIT code, a freed trampoline, an unloaded module
    Executable, // the game's own executable
    Named,      // a DLL, named by its export directory
    Unnamed,    // an image with no export name
};

// What the filter learned about the exception, without allocating.
struct Fault
{
    uint32_t code = 0;
    uint64_t address = 0; // where it was raised
    Image image = Image::None;
    const char* moduleName = nullptr; // Named: ImageExportName's sanitised copy
    uint64_t offset = 0;              // address - the image's base
    int accessKind = -1;              // access violation or in-page error: 0 read, 1 write, 8 execute; -1 none
    uint64_t accessTarget = 0;
    uint32_t thread = 0; // the faulting thread
};

// Local time, as the log's own lines carry it ("[%H:%M:%S.%f]").
struct Clock
{
    uint16_t hour = 0, minute = 0, second = 0, millisecond = 0;
};

inline const char* CodeName(uint32_t code)
{
    switch (code)
    {
    case 0xC0000005u:
        return "access violation";
    case 0xC0000006u:
        return "in-page error";
    case 0xC000001Du:
        return "illegal instruction";
    case 0xC0000094u:
        return "integer divide by zero";
    case 0xC00000FDu:
        return "stack overflow";
    case 0xC0000374u:
        return "heap corruption";
    case 0xC0000409u:
        return "stack buffer overrun or fail-fast";
    case 0xE06D7363u:
        return "C++ exception";
    case 0x80000003u:
        return "breakpoint";
    default:
        return nullptr;
    }
}

namespace Detail
{
// Appends into a fixed buffer, always terminated, never past cap; what does not fit is dropped.
struct Writer
{
    char* out;
    size_t cap;
    size_t length = 0;

    Writer(char* buffer, size_t capacity) : out(buffer), cap(capacity)
    {
        if (cap != 0)
            out[0] = '\0';
    }

    void Char(char c)
    {
        if (cap != 0 && length + 1 < cap)
        {
            out[length++] = c;
            out[length] = '\0';
        }
    }
    void Text(const char* text)
    {
        for (; text != nullptr && *text != '\0'; ++text)
            Char(*text);
    }
    void Decimal(uint64_t value)
    {
        char digits[20];
        size_t n = 0;
        do
        {
            digits[n++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (n != 0)
            Char(digits[--n]);
    }
    void Signed(int64_t value)
    {
        if (value < 0)
        {
            Char('-');
            Decimal(static_cast<uint64_t>(0) - static_cast<uint64_t>(value));
        }
        else
            Decimal(static_cast<uint64_t>(value));
    }
    // 0x and lower-case digits; width pads with zeros, 0 for as many as needed.
    void Hex(uint64_t value, int width, bool upper = false)
    {
        const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
        Text("0x");
        int n = 16;
        while (n > 1 && (value >> ((n - 1) * 4)) == 0 && n > width)
            --n;
        for (int i = n - 1; i >= 0; --i)
            Char(digits[(value >> (i * 4)) & 0xf]);
    }
    void Two(unsigned value)
    {
        Char(static_cast<char>('0' + value / 10 % 10));
        Char(static_cast<char>('0' + value % 10));
    }
};

inline void Prefix(Writer& w, const Clock& clock)
{
    w.Char('[');
    w.Two(clock.hour);
    w.Char(':');
    w.Two(clock.minute);
    w.Char(':');
    w.Two(clock.second);
    w.Char('.');
    w.Char(static_cast<char>('0' + clock.millisecond / 100 % 10));
    w.Two(clock.millisecond % 100);
    w.Text("000] [E] DLSS-NR crash note: ");
}

inline bool NameChar(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '-' || c == '+';
}

template <typename T> inline bool ReadAt(const unsigned char* base, size_t limit, size_t at, T* value)
{
    if (at > limit || limit - at < sizeof(T))
        return false;
    std::memcpy(value, base + at, sizeof(T));
    return true;
}
} // namespace Detail

// The export directory's name of a mapped PE32+ image at base, sanitised to [A-Za-z0-9._+-] (anything else becomes
// '_', so no path or separator can reach the log) and cut to cap - 1 characters. headerBytes is what is known
// readable from base (the headers); mappedBytes bounds SizeOfImage (the caller passes what it can vouch for). False,
// with out untouched, for anything that is not a well-formed image with a non-empty, terminated name.
inline bool ImageExportName(const unsigned char* base, size_t headerBytes, size_t mappedBytes, char* out, size_t cap)
{
    using Detail::ReadAt;
    if (base == nullptr || out == nullptr || cap == 0)
        return false;
    uint16_t mz = 0;
    uint32_t lfanew = 0;
    if (!ReadAt(base, headerBytes, 0, &mz) || mz != 0x5a4d || !ReadAt(base, headerBytes, 0x3c, &lfanew))
        return false;
    const size_t nt = lfanew;
    const size_t optional = nt + 24;
    uint32_t signature = 0;
    uint16_t magic = 0;
    uint32_t sizeOfImage = 0, directories = 0, exportRva = 0;
    if (!ReadAt(base, headerBytes, nt, &signature) || signature != 0x00004550 ||
        !ReadAt(base, headerBytes, optional, &magic) || magic != 0x20b ||
        !ReadAt(base, headerBytes, optional + 56, &sizeOfImage) ||
        !ReadAt(base, headerBytes, optional + 108, &directories) || directories < 1 ||
        !ReadAt(base, headerBytes, optional + 112, &exportRva) || exportRva == 0)
        return false;
    const size_t image = sizeOfImage < mappedBytes ? sizeOfImage : mappedBytes;
    uint32_t nameRva = 0;
    if (!ReadAt(base, image, size_t(exportRva) + 12, &nameRva) || nameRva == 0 || nameRva >= image)
        return false;
    // Terminated within the image and within a sane length.
    constexpr size_t kLongest = 255;
    size_t length = 0;
    while (true)
    {
        if (nameRva + length >= image || length > kLongest)
            return false;
        if (base[nameRva + length] == 0)
            break;
        ++length;
    }
    if (length == 0)
        return false;
    const size_t kept = length < cap - 1 ? length : cap - 1;
    for (size_t i = 0; i < kept; ++i)
    {
        const unsigned char c = base[nameRva + i];
        out[i] = Detail::NameChar(c) ? static_cast<char>(c) : '_';
    }
    out[kept] = '\0';
    return true;
}

// The three lines, into out (at most cap bytes, terminated). Returns the length written.
inline size_t Format(char* out, size_t cap, const Fault& fault, const Clock& clock, Funnel::StageMark stage,
                     const Funnel::Snapshot& funnel, Funnel::Route route)
{
    Detail::Writer w(out, cap);

    // The exception, where it was raised and on which thread.
    Detail::Prefix(w, clock);
    w.Text("unhandled exception ");
    w.Hex(fault.code, 8, true);
    const char* name = CodeName(fault.code);
    if (name != nullptr || fault.accessKind >= 0)
    {
        w.Text(" (");
        w.Text(name != nullptr ? name : "exception");
        if (fault.accessKind >= 0)
        {
            w.Text(fault.accessKind == 1 ? ", write at " : fault.accessKind == 8 ? ", execute at " : ", read at ");
            w.Hex(fault.accessTarget, 16);
        }
        w.Char(')');
    }
    w.Text(" at ");
    switch (fault.image)
    {
    case Image::None:
        w.Hex(fault.address, 16);
        w.Text(" (in no image)");
        break;
    case Image::Executable:
        w.Text("the game's executable+");
        w.Hex(fault.offset, 0);
        break;
    default:
        w.Text(fault.image == Image::Named && fault.moduleName != nullptr && fault.moduleName[0] != '\0'
                   ? fault.moduleName
                   : "an image without an export name");
        w.Char('+');
        w.Hex(fault.offset, 0);
        break;
    }
    w.Text(", thread ");
    w.Decimal(fault.thread);
    w.Char('\n');

    // Where NR last was.
    Detail::Prefix(w, clock);
    w.Text("last NR stage: ");
    if (stage.stage == Funnel::Stage::None)
        w.Text(Funnel::StageText(Funnel::Stage::None));
    else
    {
        w.Text(Funnel::RouteName(stage.route));
        w.Text(", ");
        w.Text(Funnel::StageText(stage.stage));
        if (stage.thread == fault.thread)
            w.Text(", on this thread");
        else
        {
            w.Text(", on another thread (");
            w.Decimal(stage.thread);
            w.Char(')');
        }
    }
    w.Char('\n');

    // The funnel, as the diagnostic report has it, in one line.
    Detail::Prefix(w, clock);
    w.Text("funnel ");
    w.Text(Funnel::RouteTag(route));
    w.Text(" eligible=");
    w.Decimal(funnel.eligible);
    w.Text(" ran=");
    w.Decimal(funnel.ran);
    w.Text(" model=");
    w.Decimal(funnel.modelCalls);
    w.Text(" declined=");
    w.Decimal(funnel.Declined());
    w.Text(" unaccounted=");
    w.Signed(funnel.Unaccounted());
    w.Text(" top=");
    Funnel::Decline top {};
    uint64_t count = 0;
    if (funnel.TopDecline(&top, &count))
    {
        w.Text(Funnel::DeclineTag(top));
        w.Char(':');
        w.Decimal(count);
    }
    else
        w.Text("none");
    w.Char('\n');

    return w.length;
}

// DlssNr_CrashNote.cpp. InstallOnce is called on every evaluate with NR on and costs one relaxed load after the first;
// the first opens a second handle on OptiScaler's log and puts the filter in place, chained to the one it replaces.
// Uninstall gives the slot back on an unload (FreeLibrary), only while it is still ours.
void InstallOnce();
void Uninstall();
} // namespace DlssNr::CrashNote
