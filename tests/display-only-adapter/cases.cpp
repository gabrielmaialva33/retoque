#include <misc/DisplayOnlyAdapter.h>
#include <cstdio>

// One fake kernel adapter per case
static struct
{
    bool openFails = false, queryFails = false, render = true, indirect = false;
    UINT queriedType = 0;
    int opens = 0, closes = 0;
} g;

static NTSTATUS Open(D3DKMT_OPENADAPTERFROMLUID* open)
{
    if (g.openFails)
        return (NTSTATUS) 0xC0000001;
    ++g.opens;
    open->hAdapter = 7;
    return 0;
}

static NTSTATUS Query(const D3DKMT_QUERYADAPTERINFO* query)
{
    g.queriedType = query->Type;
    if (g.queryFails || query->PrivateDriverDataSize != sizeof(D3DKMT_ADAPTERTYPE))
        return (NTSTATUS) 0xC0000001;
    auto* type = static_cast<D3DKMT_ADAPTERTYPE*>(query->pPrivateDriverData);
    type->RenderSupported = g.render;
    type->DisplaySupported = 1;
    type->IndirectDisplayDevice = g.indirect;
    return 0;
}

static NTSTATUS Close(const D3DKMT_CLOSEADAPTER* close)
{
    if (close->hAdapter == 7)
        ++g.closes;
    return 0;
}

static int failures = 0;
static void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

template <typename Setup> static bool Run(Setup setup)
{
    g = {};
    setup();
    return DisplayOnlyAdapter::Is(LUID { 0x1F882, 0 }, &Open, &Query, &Close);
}

int main()
{
    // Rafael's Parsec virtual display: indirect, no rendering (adapters.exe, 2026-10-29)
    Check(Run([] { g.render = false, g.indirect = true; }), "an indirect display that cannot render is display-only");
    Check(g.queriedType == KMTQAITYPE_ADAPTERTYPE && g.opens == 1 && g.closes == 1,
          "it asks KMTQAITYPE_ADAPTERTYPE and closes the adapter it opened");
    // The real RTX 3060 beside it, and an indirect display that does render
    Check(!Run([] {}), "a render adapter is kept");
    Check(!Run([] { g.indirect = true; }), "an indirect display that renders is kept");
    // Anything that cannot be asked keeps the adapter, as before
    Check(!Run([] { g.openFails = true; }), "an adapter that cannot be opened is kept");
    Check(!Run([] { g.queryFails = true, g.render = false, g.indirect = true; }) && g.closes == 1,
          "a refused query keeps the adapter and still closes it");
    Check(!DisplayOnlyAdapter::Is(LUID {}, static_cast<decltype(&Open)>(nullptr), &Query, &Close),
          "without gdi32's entry points the adapter is kept");

    if (failures != 0)
        return 1;
    std::puts("display-only-adapter: all cases pass");
    return 0;
}
