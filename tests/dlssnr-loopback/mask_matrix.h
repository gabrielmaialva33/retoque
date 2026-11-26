// Test-only ControlMask plan, shared by the GPU fixture and its portable host tests.
// Experiment motivated by MFGAdaUnlock-RenoDx 438f5dac2e48d9d73564cd167eb3289abee48b99
// (tests/nr_probe.cpp, MIT): fractional R8 masks and the ID3D12Resource* setter. No source copied.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace MaskMatrix
{
struct Trial
{
    std::string name;
    bool masked;
    bool r8;
    bool resourceSetter;
    unsigned blend;
    std::array<unsigned char, 4> rgba;
};

inline std::vector<Trial> Trials()
{
    std::vector<Trial> result;
    for (bool resourceSetter : { false, true })
        for (unsigned blend = 0; blend < 4; ++blend)
        {
            const std::string prefix = std::string(resourceSetter ? "resource" : "u64") + "-m" + std::to_string(blend);
            const auto null = [&](const char* suffix)
            { result.push_back({ prefix + suffix, false, false, resourceSetter, blend, { 0, 0, 0, 255 } }); };
            null("-null-r0");
            null("-null-r1");
            for (const std::string arm : { "r8", "r001", "r111", "rgb" })
                for (unsigned char value : { 0, 64, 128, 191, 255 })
                {
                    std::array<unsigned char, 4> rgba { value, 0, 0, 255 };
                    if (arm == "r111")
                        rgba[1] = rgba[2] = 255;
                    else if (arm == "rgb")
                        rgba[1] = rgba[2] = value;
                    result.push_back({ prefix + "-" + arm + "-v" + std::to_string(value), true, arm == "r8",
                                       resourceSetter, blend, rgba });
                }
            null("-null-after");
        }
    return result;
}

// Clear BOTH overloads first, including at null controls. A parameter implementation may keep
// typed slots separately: no trial may inherit a resource from the preceding setter arm.
// The only difference between the two full-matrix arms is the final write/read overload.
// Resource is ID3D12Resource on the GPU, a recording fake in the host test. Legacy mode does not use this.
template <class Parameters, class Resource> bool SetResource(Parameters& params, Resource* resource, bool typed)
{
    constexpr auto key = "DLSSNR.ControlMask";
    params.Set(key, 0ull);
    params.Set(key, static_cast<Resource*>(nullptr));
    if (typed)
    {
        params.Set(key, resource);
        Resource* read = nullptr;
        return static_cast<unsigned>(params.Get(key, &read)) == 1 && read == resource;
    }
    const auto value = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(resource));
    params.Set(key, value);
    unsigned long long read = ~0ull;
    return static_cast<unsigned>(params.Get(key, &read)) == 1 && read == value;
}
} // namespace MaskMatrix
