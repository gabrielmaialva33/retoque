// tests/nr-crash-note: the last-chance crash note's text (retoque/dlssnr/DlssNr_CrashNote.h,
// docs/dlssnr/design/engagement-funnel.md, "The crash note"). The header is standard library only, apart from the
// funnel it reports, and compiled as it is.

#include <dlssnr/DlssNr_CrashNote.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <vector>

// The note is written from an unhandled-exception filter, whose heap may be the thing that broke: nothing it calls may
// allocate. Every operator new in this program is counted.
static int g_allocations = 0;

void* operator new(std::size_t size)
{
    ++g_allocations;
    if (void* p = std::malloc(size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace
{
int g_failures = 0;

void Require(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

bool Contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

size_t Count(std::string_view text, std::string_view part)
{
    size_t n = 0;
    for (size_t at = text.find(part); at != std::string_view::npos; at = text.find(part, at + part.size()))
        ++n;
    return n;
}

namespace CN = DlssNr::CrashNote;
namespace F = DlssNr::Funnel;

constexpr CN::Clock kClock { 13, 5, 9, 42 };

F::Snapshot Funnel()
{
    F::Snapshot s;
    s.eligible = 1200;
    s.ran = 1100;
    s.modelCalls = 1080;
    s.declines[static_cast<size_t>(F::Decline::DeferredToPresent)] = 60;
    s.declines[static_cast<size_t>(F::Decline::PassRecordedNothing)] = 40;
    return s;
}

CN::Fault AccessViolation()
{
    CN::Fault f;
    f.code = 0xC0000005u;
    f.address = 0x7ff612345678ull;
    f.image = CN::Image::Named;
    f.moduleName = "nvngx_dlssnr.dll";
    f.offset = 0x1a2b3c;
    f.accessKind = 1;
    f.accessTarget = 0x10;
    f.thread = 1234;
    return f;
}

std::string Note(const CN::Fault& fault, F::StageMark mark, const F::Snapshot& snapshot)
{
    char buffer[CN::kNoteBytes];
    const size_t n = CN::Format(buffer, sizeof(buffer), fault, kClock, mark, snapshot, F::Route::D3d12AfterUpscale);
    Require(n < sizeof(buffer) && buffer[n] == '\0', "the note is terminated inside its buffer");
    return std::string(buffer, n);
}

void ThreeLinesInTheLogsShape()
{
    const auto text = Note(AccessViolation(), { F::Route::D3d12AfterUpscale, F::Stage::Recording, 1234 }, Funnel());
    Require(Count(text, "\n") == 3 && text.back() == '\n', "three lines, each ended: " + text);
    Require(Count(text, "[13:05:09.042000] [E] DLSS-NR crash note: ") == 3,
            "every line carries the log's own time and level prefix: " + text);
    Require(Contains(text, "unhandled exception 0xC0000005 (access violation, write at 0x0000000000000010)"),
            "the code, its name and the access: " + text);
    Require(Contains(text, " at nvngx_dlssnr.dll+0x1a2b3c"), "the module and the offset: " + text);
    Require(Contains(text, "thread 1234"), "the crashing thread: " + text);
    Require(Contains(text, "last NR stage: D3D12 after the upscaler, recording the pass, on this thread"),
            "the last stage, on the crashing thread: " + text);
    Require(Contains(text, "funnel d3d12_after_upscale eligible=1200 ran=1100 model=1080 declined=100 "
                           "unaccounted=0 top=deferred_to_present:60"),
            "the funnel snapshot: " + text);
    Require(!Contains(text, "\\") && !Contains(text, ":/") && !Contains(text, "/home"), "no path: " + text);
}

void Codes()
{
    const struct
    {
        uint32_t code;
        const char* name;
    } known[] = { { 0xC0000005u, "access violation" },      { 0xC0000409u, "stack buffer overrun or fail-fast" },
                  { 0xC000001Du, "illegal instruction" },   { 0xC0000374u, "heap corruption" },
                  { 0xE06D7363u, "C++ exception" },         { 0xC0000006u, "in-page error" },
                  { 0xC0000094u, "integer divide by zero" } };
    for (const auto& k : known)
        Require(std::string_view(CN::CodeName(k.code)) == k.name, std::string("code name for ") + k.name);
    Require(CN::CodeName(0x12345678u) == nullptr, "an unknown code has no name");

    auto f = AccessViolation();
    f.code = 0x12345678u;
    f.accessKind = -1;
    const auto text = Note(f, {}, {});
    Require(Contains(text, "unhandled exception 0x12345678 at"), "an unknown code is the hex alone: " + text);

    f = AccessViolation();
    f.accessKind = 8;
    Require(Contains(Note(f, {}, {}), "(access violation, execute at 0x"), "an execute access");
    f.accessKind = 0;
    Require(Contains(Note(f, {}, {}), "(access violation, read at 0x"), "a read access");
}

void Images()
{
    auto f = AccessViolation();
    f.image = CN::Image::Executable;
    f.moduleName = nullptr;
    f.offset = 0x42;
    Require(Contains(Note(f, {}, {}), " at the game's executable+0x42,"), "the executable is named as such");

    f.image = CN::Image::Unnamed;
    Require(Contains(Note(f, {}, {}), " at an image without an export name+0x42,"), "an unnamed image");

    f.image = CN::Image::None;
    f.address = 0xdeadbeef;
    Require(Contains(Note(f, {}, {}), " at 0x00000000deadbeef (in no image),"), "an address outside every image");

    f.image = CN::Image::Named;
    f.moduleName = nullptr;
    Require(Contains(Note(f, {}, {}), " at an image without an export name+0x42,"), "a named image with no name");
}

void Stages()
{
    const auto other = Note(AccessViolation(), { F::Route::D3d12AfterUpscale, F::Stage::Left, 99 }, Funnel());
    Require(Contains(other, "last NR stage: D3D12 after the upscaler, left the pass, on another thread (99)"),
            "a stage on another thread names it: " + other);
    const auto none = Note(AccessViolation(), {}, Funnel());
    Require(Contains(none, "last NR stage: none recorded"), "no stage yet: " + none);
}

void FunnelLine()
{
    const auto quiet = Note(AccessViolation(), {}, F::Snapshot {});
    Require(Contains(quiet, "funnel d3d12_after_upscale eligible=0 ran=0 model=0 declined=0 unaccounted=0 top=none"),
            "an empty funnel: " + quiet);

    F::Snapshot leak;
    leak.eligible = 5;
    leak.ran = 3;
    Require(Contains(Note(AccessViolation(), {}, leak), "unaccounted=2 top=none"),
            "unaccounted is eligible - ran - declined");
}

void NeverOverrunsTheBuffer()
{
    const auto fault = AccessViolation();
    const F::StageMark mark { F::Route::D3d12AfterUpscale, F::Stage::Recording, 1234 };
    for (size_t cap : { size_t(0), size_t(1), size_t(2), size_t(16), size_t(40), size_t(200), CN::kNoteBytes })
    {
        std::vector<char> buffer(cap + 8, '#');
        const size_t n = CN::Format(buffer.data(), cap, fault, kClock, mark, Funnel(), F::Route::D3d12AfterUpscale);
        Require(cap == 0 ? n == 0 : n < cap && buffer[n] == '\0',
                "a short buffer is filled and terminated, cap " + std::to_string(cap));
        for (size_t i = cap; i < buffer.size(); ++i)
            Require(buffer[i] == '#', "nothing written past the buffer, cap " + std::to_string(cap));
    }
    Require(Note(fault, mark, Funnel()).size() < CN::kNoteBytes - 1, "the full note fits kNoteBytes with room left");
    Require(std::strlen(CN::kStackOverflowLine) > 0 &&
                CN::kStackOverflowLine[std::strlen(CN::kStackOverflowLine) - 1] == '\n',
            "the stack-overflow line is one ended line");
    Require(Contains(CN::kStackOverflowLine, "0xC00000FD"), "the stack-overflow line names its code");
}

// A minimal PE32+ image: DOS header, NT headers, one export directory and its name.
std::vector<unsigned char> Image(const char* name, uint32_t sizeOfImage = 0x400)
{
    std::vector<unsigned char> image(0x400, 0);
    const auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&image[at], &v, 2); };
    const auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&image[at], &v, 4); };
    image[0] = 'M';
    image[1] = 'Z';
    put32(0x3c, 0x80);       // e_lfanew
    put32(0x80, 0x00004550); // "PE\0\0"
    put16(0x84, 0x8664);     // Machine
    put16(0x94, 0xf0);       // SizeOfOptionalHeader
    put16(0x98, 0x20b);      // PE32+ magic
    put32(0x98 + 56, sizeOfImage);
    put32(0x98 + 108, 16);    // NumberOfRvaAndSizes
    put32(0x98 + 112, 0x200); // export directory RVA
    put32(0x98 + 116, 0x28);  // and size
    put32(0x200 + 12, 0x300); // IMAGE_EXPORT_DIRECTORY::Name
    std::memcpy(&image[0x300], name, std::strlen(name) + 1);
    return image;
}

std::string ExportName(const std::vector<unsigned char>& image, size_t headerBytes, size_t mapped, size_t cap = 64)
{
    std::vector<char> out(cap + 4, '#');
    if (!CN::ImageExportName(image.data(), headerBytes, mapped, out.data(), cap))
        return "<none>";
    for (size_t i = cap; i < out.size(); ++i)
        Require(out[i] == '#', "the export name stays inside its buffer");
    return out.data();
}

void ExportNames()
{
    auto image = Image("nvngx_dlssnr.dll");
    Require(ExportName(image, image.size(), image.size()) == "nvngx_dlssnr.dll", "the export name of a PE32+ image");
    Require(ExportName(image, image.size(), image.size(), 6) == "nvngx", "a long name is cut to its buffer");
    Require(ExportName(image, 0x40, image.size()) == "<none>", "NT headers past the readable header are refused");

    auto bad = image;
    bad[0] = 'X';
    Require(ExportName(bad, bad.size(), bad.size()) == "<none>", "no MZ, no name");
    bad = image;
    bad[0x80] = 'X';
    Require(ExportName(bad, bad.size(), bad.size()) == "<none>", "no PE signature, no name");
    bad = image;
    const uint16_t pe32 = 0x10b;
    std::memcpy(&bad[0x98], &pe32, 2);
    Require(ExportName(bad, bad.size(), bad.size()) == "<none>", "a 32-bit image is not read as 64-bit");
    bad = image;
    const uint32_t far = 0x10000;
    std::memcpy(&bad[0x98 + 112], &far, 4);
    Require(ExportName(bad, bad.size(), bad.size()) == "<none>", "an export directory past the image is refused");
    bad = image;
    std::memcpy(&bad[0x200 + 12], &far, 4);
    Require(ExportName(bad, bad.size(), bad.size()) == "<none>", "a name past the image is refused");
    bad = image;
    const uint32_t zero = 0;
    std::memcpy(&bad[0x98 + 112], &zero, 4);
    Require(ExportName(bad, bad.size(), bad.size()) == "<none>", "no export directory, no name");

    auto big = Image("x.dll", 0x10000);
    Require(ExportName(big, big.size(), 0x300) == "<none>", "SizeOfImage is bounded by what is mapped");

    auto unterminated = Image("abc");
    std::memset(&unterminated[0x300], 'a', 0x100);
    Require(ExportName(unterminated, unterminated.size(), unterminated.size()) == "<none>",
            "a name that runs off the image is refused");

    // A synthetic user folder, split so the source holds no personal-looking path (repo-privacy).
    auto pathy = Image("C:\\Users\\"
                       "someone\\evil/x.dll");
    const auto cleaned = ExportName(pathy, pathy.size(), pathy.size());
    Require(cleaned != "<none>" && !Contains(cleaned, "\\") && !Contains(cleaned, "/") && !Contains(cleaned, ":"),
            "an export name can never carry a path into the log: " + cleaned);

    Require(ExportName(Image(""), 0x400, 0x400) == "<none>", "an empty name is no name");
}

void NothingAllocates()
{
    const auto fault = AccessViolation();
    const auto snapshot = Funnel();
    auto image = Image("nvngx_dlssnr.dll");
    char buffer[CN::kNoteBytes];
    char name[64];
    const int before = g_allocations;
    CN::Format(buffer, sizeof(buffer), fault, kClock, { F::Route::D3d12AfterUpscale, F::Stage::Recording, 1 }, snapshot,
               F::Route::D3d12AfterUpscale);
    CN::ImageExportName(image.data(), image.size(), image.size(), name, sizeof(name));
    (void) CN::CodeName(0xC0000005u);
    (void) F::LastStage();
    (void) F::Read(F::Route::D3d12AfterUpscale);
    const bool none = g_allocations == before; // read before Require's message is built, which allocates
    Require(none, "formatting the note, naming the image and reading the funnel allocate nothing");
}
} // namespace

int main()
{
    ThreeLinesInTheLogsShape();
    Codes();
    Images();
    Stages();
    FunnelLine();
    NeverOverrunsTheBuffer();
    ExportNames();
    NothingAllocates();

    if (g_failures != 0)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS: crash note cases\n");
    return 0;
}
