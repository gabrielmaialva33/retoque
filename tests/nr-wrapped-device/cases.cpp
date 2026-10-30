// DlssNr_DeviceMatch.h's rule, and the production DeviceChildBelongsTo pair compiled against it, with fake devices
// that model a wrapper layer the way ReShade's D3D12 proxies behave: the wrapper is its own interface on the same
// adapter, its command lists report it, and the resources it creates report the device underneath. No D3D12.
#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "retoque/dlssnr/DlssNr_DeviceMatch.h"

using HRESULT = int;
#define FAILED(x) ((x) < 0)
#define SUCCEEDED(x) ((x) >= 0)
#define IID_PPV_ARGS(x) (x)
inline std::vector<std::string> logs;
#define LOG_INFO(...) logs.emplace_back("info")

struct LUID
{
    unsigned long LowPart = 0;
    long HighPart = 0;
};
struct ID3D12Device
{
    LUID luid;
    unsigned refs = 1;
    LUID GetAdapterLuid() { return luid; }
    unsigned AddRef() { return ++refs; }
    unsigned Release()
    {
        assert(refs > 1 && "a device reference released that was never taken");
        return --refs;
    }
};
struct ID3D12DeviceChild
{
    ID3D12Device* device = nullptr;
    virtual ~ID3D12DeviceChild() = default;
    HRESULT GetDevice(ID3D12Device** out)
    {
        *out = device;
        if (device == nullptr)
            return -1;
        device->AddRef();
        return 0;
    }
};
struct ID3D12Resource : ID3D12DeviceChild
{
};
struct ID3D12GraphicsCommandList : ID3D12DeviceChild
{
};

#include "artifacts/belongs.inc"

using DlssNr::DeviceMatch::Relate;
using DlssNr::DeviceMatch::Relation;

int main()
{
    unsigned checks = 0;
    ID3D12Device real { { 0x1F882, 0 } }, wrapper { { 0x1F882, 0 } }, other { { 0x2A000, 0 } },
        highOnly { { 0x1F882, 1 } }, unknown { { 0, 0 } }, unknownToo { { 0, 0 } };

    // The rule.
    assert(Relate(&real, &real) == Relation::Same);
    assert(Relate(&wrapper, &real) == Relation::Wrapper && Relate(&real, &wrapper) == Relation::Wrapper);
    assert(Relate(&real, &other) == Relation::Foreign);
    assert(Relate(&real, &highOnly) == Relation::Foreign && "both halves of the LUID");
    assert(Relate(&unknown, &unknownToo) == Relation::Foreign && "a zero LUID proves nothing");
    assert(Relate(&real, &unknown) == Relation::Foreign && Relate(&unknown, &real) == Relation::Foreign);
    assert(Relate(&unknown, &unknown) == Relation::Same);
    assert(Relate<ID3D12Device>(nullptr, &real) == Relation::Foreign);
    assert(Relate<ID3D12Device>(&real, nullptr) == Relation::Foreign);
    checks += 9;

    // Lists and queues: the exact device, never a wrapper of it. The list's device is what NGX dereferences.
    ID3D12GraphicsCommandList onWrapper, onReal, onOther, orphan;
    onWrapper.device = &wrapper;
    onReal.device = &real;
    onOther.device = &other;
    assert(DeviceChildBelongsTo(&wrapper, &onWrapper));
    assert(!DeviceChildBelongsTo(&wrapper, &onReal) && !DeviceChildBelongsTo(&real, &onWrapper));
    assert(!DeviceChildBelongsTo(&wrapper, &onOther) && !DeviceChildBelongsTo(&wrapper, &orphan));
    assert(!DeviceChildBelongsTo(nullptr, &onWrapper));
    checks += 4;

    // Resources: the device, or the one it wraps (or that wraps it) on the same adapter; never another adapter.
    ID3D12Resource texture, foreign, nowhere;
    texture.device = &real;
    foreign.device = &other;
    assert(DeviceChildBelongsTo(&real, &texture));
    assert(logs.empty() && "the plain case says nothing");
    assert(DeviceChildBelongsTo(&wrapper, &texture));
    assert(logs.size() == 1 && "the wrapper is said once");
    for (int i = 0; i < 100; ++i)
        assert(DeviceChildBelongsTo(&wrapper, &texture));
    assert(logs.size() == 1);
    assert(!DeviceChildBelongsTo(&wrapper, &foreign) && !DeviceChildBelongsTo(&real, &foreign));
    assert(!DeviceChildBelongsTo(&wrapper, &nowhere) && !DeviceChildBelongsTo(&wrapper, (ID3D12Resource*) nullptr));
    ID3D12Resource zero;
    zero.device = &unknownToo;
    assert(!DeviceChildBelongsTo(&unknown, &zero));
    // A resource seen through its base type keeps the strict rule: only the declared resource overload relaxes it.
    assert(!DeviceChildBelongsTo(&wrapper, static_cast<ID3D12DeviceChild*>(&texture)));
    checks += 7;

    // Every reference the queries took was given back.
    for (auto* device : { &real, &wrapper, &other, &highOnly, &unknown, &unknownToo })
        assert(device->refs == 1);
    ++checks;

    assert(checks == 21 && "ZERO COVERAGE");
    std::printf("PASS: %u device-relation cases\n", checks);
}
