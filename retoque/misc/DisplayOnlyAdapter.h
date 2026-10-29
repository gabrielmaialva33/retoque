#pragma once

// An indirect display adapter (Parsec's virtual display, a streaming or remote-desktop display driver) is enumerated by
// DXGI with the name, vendor and device id of the GPU it renders with, but it cannot render itself and NVAPI does not
// know its LUID. On Rafael's RTX 3060 (2026-10-29) such an adapter came first in DXGI's order, ahead of the real GPU:
// as IdentifyGpu's first adapter it became the "primary GPU", with no NVIDIA architecture and so no DLSS. The kernel
// says what it is: KMTQAITYPE_ADAPTERTYPE, IndirectDisplayDevice set and RenderSupported clear. Anything that cannot be
// asked counts as a normal adapter, as before. Idea from the 2026-10-04 survey (C-fg-display.md, item 6).

#include <windows.h>
#include <d3dkmthk.h>

namespace DisplayOnlyAdapter
{

// True only when the kernel says the adapter is an indirect display that does not render
template <typename Open, typename Query, typename Close>
bool Is(LUID luid, Open openAdapter, Query queryAdapterInfo, Close closeAdapter)
{
    if (openAdapter == nullptr || queryAdapterInfo == nullptr || closeAdapter == nullptr)
        return false;

    D3DKMT_OPENADAPTERFROMLUID open {};
    open.AdapterLuid = luid;

    if (openAdapter(&open) != 0)
        return false;

    D3DKMT_ADAPTERTYPE type {};
    D3DKMT_QUERYADAPTERINFO query {};
    query.hAdapter = open.hAdapter;
    query.Type = KMTQAITYPE_ADAPTERTYPE;
    query.pPrivateDriverData = &type;
    query.PrivateDriverDataSize = sizeof(type);

    const bool known = queryAdapterInfo(&query) == 0;

    D3DKMT_CLOSEADAPTER close {};
    close.hAdapter = open.hAdapter;
    closeAdapter(&close);

    return known && type.IndirectDisplayDevice && !type.RenderSupported;
}

} // namespace DisplayOnlyAdapter
