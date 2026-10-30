#pragma once

// Which D3D12 device a piece of NR work belongs to, when a layer wraps the device.
//
// A wrapper loaded between the game and OptiScaler -- ReShade through [Plugins] LoadReShade, a Streamline proxy --
// hands the game a device and command lists of its own and forwards them to the real ones. It does not wrap
// resources, so a texture reports the real device while the command list it is recorded on reports the wrapper.
// NGX and D3D12Core dereference the device behind the list being recorded: a model created on the texture's device
// and evaluated on the wrapper's list faulted inside D3D12Core (or hung with the model before the upscaler) in
// jlrouzies-fr/OptiScaler-DLSSNR-PreSR-Multipass, which followed the list's device instead (1c63318d, and 6724d677 for
// a queue that is another wrapper of the same device; GPL-3.0, adapted here).
//
// The NR routes therefore take their device from the command list they record on (or the queue they submit to) and
// hold every list and queue to that exact device. A resource may report that device or another one on the same
// adapter: D3D12 keeps one device per adapter in a process, so a second interface with the same LUID is a wrapper of
// it, not an independent device. A zero LUID proves nothing and is refused. docs/dlssnr/design/dx12-robustness.md,
// tested by tests/nr-wrapped-device.
//
// No Windows header: the device type is a template parameter, so host fakes compile the same rule.
namespace DlssNr::DeviceMatch
{
enum class Relation
{
    Same,    // the same interface
    Wrapper, // another interface on the same adapter: the device behind a wrapper layer, or the wrapper itself
    Foreign, // no device, a different adapter, or nothing to prove the adapter by
};

template <class Device> Relation Relate(Device* expected, Device* reported)
{
    if (expected == nullptr || reported == nullptr)
        return Relation::Foreign;
    if (expected == reported)
        return Relation::Same;

    const auto a = expected->GetAdapterLuid();
    const auto b = reported->GetAdapterLuid();
    if ((a.LowPart == 0 && a.HighPart == 0) || (b.LowPart == 0 && b.HighPart == 0))
        return Relation::Foreign;
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart ? Relation::Wrapper : Relation::Foreign;
}
} // namespace DlssNr::DeviceMatch
