// The D3DKMT surface retoque/misc/DisplayOnlyAdapter.h uses, laid out as the SDK declares the fields it touches.
#pragma once
using D3DKMT_HANDLE = UINT;
struct D3DKMT_OPENADAPTERFROMLUID
{
    LUID AdapterLuid;
    D3DKMT_HANDLE hAdapter;
};
enum KMTQUERYADAPTERINFOTYPE
{
    KMTQAITYPE_UMDRIVERPRIVATE = 0,
    KMTQAITYPE_ADAPTERTYPE = 15
};
struct D3DKMT_QUERYADAPTERINFO
{
    D3DKMT_HANDLE hAdapter;
    KMTQUERYADAPTERINFOTYPE Type;
    void* pPrivateDriverData;
    UINT PrivateDriverDataSize;
};
struct D3DKMT_ADAPTERTYPE
{
    UINT RenderSupported : 1;
    UINT DisplaySupported : 1;
    UINT SoftwareDevice : 1;
    UINT PostDevice : 1;
    UINT HybridDiscrete : 1;
    UINT HybridIntegrated : 1;
    UINT IndirectDisplayDevice : 1;
    UINT Paravirtualized : 1;
    UINT Reserved : 24;
};
struct D3DKMT_CLOSEADAPTER
{
    D3DKMT_HANDLE hAdapter;
};
