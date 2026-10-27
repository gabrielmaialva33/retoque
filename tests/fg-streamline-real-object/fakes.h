// Stand-ins for the COM, Streamline and logging names the extracted production code uses. The proxy answers
// Streamline's retrieve-base query the way sl.interposer's DXGIFactory::QueryInterface does: the base, AddRef'd.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

typedef long HRESULT;
#define S_OK ((HRESULT) 0)
#define E_NOINTERFACE ((HRESULT) 0x80004002L)
#define LOG_INFO(...) ((void) 0)
#define LOG_FUNC() ((void) 0)
#define __FUNCTION__ "test"

struct GUID
{
    unsigned long Data1 = 0;
    unsigned short Data2 = 0, Data3 = 0;
    unsigned char Data4[8] = {};
    bool operator==(const GUID& o) const { return std::memcmp(this, &o, sizeof(GUID)) == 0; }
};
typedef const GUID& REFIID;

static const GUID kStreamlineRiid { 0xADEC44E2, 0x61F0, 0x45C3, { 0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF } };

inline HRESULT IIDFromString(const wchar_t* text, GUID* out)
{
    if (std::wcscmp(text, L"{ADEC44E2-61F0-45C3-AD9F-1B37379284FF}") != 0)
        return E_NOINTERFACE;
    *out = kStreamlineRiid;
    return S_OK;
}

struct IUnknown
{
    long refs = 1;
    IUnknown* base = nullptr; // set on a Streamline proxy
    virtual ~IUnknown() = default;
    long AddRef() { return ++refs; }
    long Release()
    {
        if (refs <= 0)
        {
            std::fprintf(stderr, "Release on an object with no references left\n");
            std::abort();
        }
        return --refs;
    }
    HRESULT QueryInterface(REFIID riid, void** out)
    {
        if (base != nullptr && riid == kStreamlineRiid)
        {
            *out = base;
            base->AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
};
struct IDXGIFactory : IUnknown
{
};

namespace StreamlineProxy
{
inline int upgrades = 0;
inline auto UpgradeInterface()
{
    return [](void**) { ++upgrades; };
}
} // namespace StreamlineProxy

static int hookCalls = 0;
static bool hooked = false;
