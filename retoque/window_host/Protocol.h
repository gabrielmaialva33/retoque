// RetoqueWindowHost's launch and IPC contract: the command line, the exit codes, the JSON lines it writes to stdout
// and the commands it reads from stdin. Pure and Windows-free, held by tests/nr-window-host-rules; the app's
// "NR on any window" screen (phase 4) is written against this, and the README's table repeats it.
//
// stdout: one JSON object per line, each with an "event". stdin: one JSON object per line with a "cmd", optionally
// a numeric "rev" the answering event echoes. Nothing listens on a network. Unknown commands are answered, never
// acted on.

#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace RetoqueWindowHost::Protocol
{
// The host's own process environment, set before it loads OptiScaler.dll (DlssNr_WindowHost.h has the same name).
inline constexpr char kSeamEnvironment[] = "RETOQUE_WINDOW_HOST";
inline constexpr uint32_t kSeamVersion = 1;
inline constexpr uint32_t kProtocolVersion = 1;
inline constexpr size_t kMaxCommandBytes = 4096;

enum class Exit : int
{
    Ok = 0,              // stopped by the user, a command, --max-frames or the source closing
    Usage = 2,           // the command line was refused
    Unsupported = 3,     // no Windows Graphics Capture, or no usable D3D11/D3D12 device
    SourceRefused = 4,   // the chosen window may not be captured (the event says why)
    SeamRefused = 5,     // OptiScaler.dll missing, its RetoqueWindow_* exports missing or refusing
    DeviceFailed = 6,    // a device or swapchain could not be created
    RestartRequired = 7, // GPU completion could not be proved or the device was lost; objects were abandoned
};

enum class Mode
{
    Help,
    Catalog,
    Run,
};

struct Options
{
    Mode mode = Mode::Help;
    uint64_t window = 0;    // --window 0x...: a top-level window handle
    int countdown = 0;      // --countdown N: take the foreground window after N seconds
    uint64_t maxFrames = 0; // 0: until stopped
    bool trace = false;     // one event per processed frame
    bool noNr = false;      // capture and preview only; OptiScaler.dll is not loaded
    bool vsync = true;      // --no-vsync presents without waiting for the display
};

struct Parsed
{
    bool ok = false;
    Options options;
    std::string error;
};

inline bool ParseUnsigned(std::string_view text, uint64_t& value, int base)
{
    if (text.empty() || text.size() > 20)
        return false;
    uint64_t result = 0;
    for (char c : text)
    {
        int digit = -1;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        if (digit < 0 || digit >= base)
            return false;
        if (result > (UINT64_MAX - uint64_t(digit)) / uint64_t(base))
            return false;
        result = result * uint64_t(base) + uint64_t(digit);
    }
    value = result;
    return true;
}

inline Parsed ParseArguments(const std::vector<std::string>& args)
{
    Parsed parsed;
    auto fail = [&](std::string why)
    {
        parsed.ok = false;
        parsed.error = std::move(why);
        return parsed;
    };
    bool catalog = false, run = false, help = false, haveWindow = false, haveCountdown = false;
    for (size_t i = 0; i < args.size(); ++i)
    {
        const std::string& a = args[i];
        auto value = [&]() -> const std::string* { return i + 1 < args.size() ? &args[++i] : nullptr; };
        if (a == "--help" || a == "-h")
            help = true;
        else if (a == "--catalog")
            catalog = true;
        else if (a == "--run")
            run = true;
        else if (a == "--trace")
            parsed.options.trace = true;
        else if (a == "--no-nr")
            parsed.options.noNr = true;
        else if (a == "--no-vsync")
            parsed.options.vsync = false;
        else if (a == "--window")
        {
            const std::string* v = value();
            if (v == nullptr)
                return fail("--window needs a window handle, for example 0x1A2B3C");
            std::string_view text = *v;
            if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
                text.remove_prefix(2);
            else
                return fail("--window takes a hexadecimal handle starting with 0x");
            if (!ParseUnsigned(text, parsed.options.window, 16) || parsed.options.window == 0)
                return fail("--window is not a valid handle");
            haveWindow = true;
        }
        else if (a == "--countdown")
        {
            const std::string* v = value();
            uint64_t seconds = 0;
            if (v == nullptr || !ParseUnsigned(*v, seconds, 10) || seconds < 1 || seconds > 60)
                return fail("--countdown takes 1 to 60 seconds");
            parsed.options.countdown = int(seconds);
            haveCountdown = true;
        }
        else if (a == "--max-frames")
        {
            const std::string* v = value();
            if (v == nullptr || !ParseUnsigned(*v, parsed.options.maxFrames, 10) || parsed.options.maxFrames == 0)
                return fail("--max-frames takes a positive count");
        }
        else
            return fail("unknown argument: " + a);
    }
    if (help || (!catalog && !run))
    {
        if (catalog || run || haveWindow || haveCountdown)
            return fail("--help takes no other arguments");
        parsed.ok = true;
        parsed.options.mode = Mode::Help;
        return parsed;
    }
    if (catalog && run)
        return fail("--catalog and --run are separate invocations");
    if (catalog)
    {
        if (haveWindow || haveCountdown || parsed.options.maxFrames != 0 || parsed.options.trace ||
            parsed.options.noNr || !parsed.options.vsync)
            return fail("--catalog takes no other arguments");
        parsed.ok = true;
        parsed.options.mode = Mode::Catalog;
        return parsed;
    }
    if (haveWindow == haveCountdown)
        return fail("--run needs exactly one of --window HANDLE or --countdown SECONDS");
    parsed.ok = true;
    parsed.options.mode = Mode::Run;
    return parsed;
}

// --- stdout: events --------------------------------------------------------------------------------------------------

using Value = std::variant<std::string, const char*, int64_t, uint64_t, double, bool>;

struct Field
{
    const char* key;
    Value value;
};

inline void AppendEscaped(std::string& out, std::string_view text)
{
    out += '"';
    for (const char c : text)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (u < 0x20 || u == 0x7F)
            {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", unsigned(u));
                out += escaped;
            }
            else
                out += c; // UTF-8 passes through
        }
    }
    out += '"';
}

// One line, ending in '\n'. Every event carries its name first; doubles are finite (a non-finite one is written as
// null, which JSON allows and a NaN is not).
inline std::string Event(const char* name, const std::vector<Field>& fields = {})
{
    std::string out = "{\"event\":";
    AppendEscaped(out, name);
    for (const Field& field : fields)
    {
        out += ',';
        AppendEscaped(out, field.key);
        out += ':';
        if (const auto* s = std::get_if<std::string>(&field.value))
            AppendEscaped(out, *s);
        else if (const auto* cs = std::get_if<const char*>(&field.value))
        {
            if (*cs == nullptr)
                out += "null";
            else
                AppendEscaped(out, *cs);
        }
        else if (const auto* i = std::get_if<int64_t>(&field.value))
            out += std::to_string(*i);
        else if (const auto* u = std::get_if<uint64_t>(&field.value))
            out += std::to_string(*u);
        else if (const auto* d = std::get_if<double>(&field.value))
        {
            if (*d == *d && *d < 1e300 && *d > -1e300)
            {
                char number[32];
                std::snprintf(number, sizeof(number), "%.3f", *d);
                out += number;
            }
            else
                out += "null";
        }
        else if (const auto* b = std::get_if<bool>(&field.value))
            out += *b ? "true" : "false";
    }
    out += "}\n";
    return out;
}

inline std::string HandleText(uint64_t handle)
{
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(handle));
    return text;
}

// --- stdin: commands -------------------------------------------------------------------------------------------------

enum class Command
{
    Invalid, // not one flat JSON object
    Unknown, // a well-formed object whose "cmd" this host does not know
    Status,  // answer with a status event
    Stop,    // end the session and exit 0
};

struct Request
{
    Command command = Command::Invalid;
    bool haveRevision = false;
    uint64_t revision = 0;
    std::string name;  // the "cmd" as sent
    std::string error; // for Invalid
};

// A flat object of string, unsigned-integer and boolean members; anything else (nesting, arrays, floats, escapes
// other than \" and \\) is refused rather than half-understood.
inline Request ParseCommand(std::string_view line)
{
    Request request;
    auto invalid = [&](const char* why)
    {
        request.command = Command::Invalid;
        request.error = why;
        return request;
    };
    if (line.size() > kMaxCommandBytes)
        return invalid("command longer than 4096 bytes");
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
        line.remove_suffix(1);
    size_t i = 0;
    auto skip = [&]
    {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            ++i;
    };
    auto readString = [&](std::string& out) -> bool
    {
        if (i >= line.size() || line[i] != '"')
            return false;
        ++i;
        while (i < line.size() && line[i] != '"')
        {
            char c = line[i++];
            if (static_cast<unsigned char>(c) < 0x20)
                return false;
            if (c == '\\')
            {
                if (i >= line.size() || (line[i] != '"' && line[i] != '\\'))
                    return false;
                c = line[i++];
            }
            out += c;
        }
        if (i >= line.size())
            return false;
        ++i;
        return true;
    };
    std::map<std::string, std::string> strings;
    skip();
    if (i >= line.size() || line[i] != '{')
        return invalid("not a JSON object");
    ++i;
    skip();
    bool first = true;
    while (i < line.size() && line[i] != '}')
    {
        if (!first)
        {
            if (line[i] != ',')
                return invalid("expected a comma");
            ++i;
            skip();
        }
        first = false;
        std::string key;
        if (!readString(key))
            return invalid("expected a quoted member name");
        skip();
        if (i >= line.size() || line[i] != ':')
            return invalid("expected a colon");
        ++i;
        skip();
        if (i < line.size() && line[i] == '"')
        {
            std::string value;
            if (!readString(value))
                return invalid("unterminated string");
            if (strings.count(key) != 0)
                return invalid("a member appears twice");
            strings[key] = value;
        }
        else if (i < line.size() && line[i] >= '0' && line[i] <= '9')
        {
            const size_t start = i;
            while (i < line.size() && line[i] >= '0' && line[i] <= '9')
                ++i;
            uint64_t number = 0;
            if (!ParseUnsigned(line.substr(start, i - start), number, 10))
                return invalid("number out of range");
            if (key == "rev")
            {
                if (request.haveRevision)
                    return invalid("a member appears twice");
                request.haveRevision = true;
                request.revision = number;
            }
        }
        else if (line.substr(i, 4) == "true")
            i += 4;
        else if (line.substr(i, 5) == "false")
            i += 5;
        else
            return invalid("only strings, unsigned integers and booleans are understood");
        skip();
    }
    if (i >= line.size() || line[i] != '}')
        return invalid("unterminated object");
    ++i;
    skip();
    if (i != line.size())
        return invalid("text after the object");
    const auto cmd = strings.find("cmd");
    if (cmd == strings.end())
        return invalid("no \"cmd\" member");
    request.name = cmd->second;
    if (request.name == "status")
        request.command = Command::Status;
    else if (request.name == "stop" || request.name == "quit")
        request.command = Command::Stop;
    else
        request.command = Command::Unknown;
    return request;
}

inline constexpr char kUsage[] =
    "RetoqueWindowHost: NR on any window, by capture (Windows 10 2004 or later, NVIDIA GPU).\n"
    "  RetoqueWindowHost --catalog                 list the windows that may be captured, one JSON line each\n"
    "  RetoqueWindowHost --run --window 0xHANDLE   capture that window into a preview window with NR\n"
    "  RetoqueWindowHost --run --countdown N       capture the foreground window after N seconds (1-60)\n"
    "Options for --run: --max-frames N, --trace (one event per frame), --no-nr (no OptiScaler.dll),\n"
    "  --no-vsync. stdin takes {\"cmd\":\"status\"} and {\"cmd\":\"stop\"}; stdout is JSON lines.\n"
    "OptiScaler.dll, nvngx.dll_dlssnr.dll and OptiScaler.ini come from this exe's own folder, never a game's.\n";
} // namespace RetoqueWindowHost::Protocol
