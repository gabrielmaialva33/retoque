#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>
#include <dlssnr/DlssNr_VkRecording.h>

// Production payload declarations and FSR CPU constants calculation.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-source-encoding"
#endif
#include <shaders/output_scaling/OS_Common.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#define A_CPU
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <shaders/output_scaling/fsr1/ffx_fsr1.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

enum class Scaler : uint32_t
{
    Bicubic,
    CatmullRom,
    Lanczos2,
    Lanczos3,
    Kaiser2,
    Kaiser3,
    Magic,
    FSR1,
    Count
};
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include <shaders/output_scaling/OS_Vk.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#if TEST_RECORDING_SCALER
#include <shaders/dlssnr/DlssNr_Scaler_Vk.h>
#endif
#if __has_include(<shaders/dlssnr/precompile/DlssNr_Scaler_Shaders_Vk.h>)
#include <shaders/dlssnr/precompile/DlssNr_Scaler_Shaders_Vk.h>
#define TEST_NR_RGBA_SHADERS 1
#else
#define TEST_NR_RGBA_SHADERS 0
#endif
#define LOG_ERROR(...) ((void) 0)
#define LOG_INFO(...) ((void) 0)
#define LOG_FUNC(...) ((void) 0)

namespace boundary
{
uint64_t nextHandle = 100;
template <class T> T handle(uint64_t value) { return (T) (uintptr_t) value; }
template <class T> uint64_t id(T value) { return (uint64_t) (uintptr_t) value; }
template <class T> T fresh() { return handle<T>(++nextHandle); }
std::map<uint64_t, std::vector<unsigned char>> memory;
std::map<uint64_t, uint64_t> buffers;
std::map<uint64_t, VkDeviceSize> bufferSizes;
struct Descriptor
{
    VkDescriptorBufferInfo uniform {};
    VkImageView source {}, target {};
};
std::map<uint64_t, Descriptor> descriptors;
struct Command
{
    VkDescriptorSet set;
    uint32_t x, y;
};
std::map<uint64_t, VkDescriptorSet> bound;
std::map<uint64_t, std::vector<Command>> commands;
size_t effects = 0;
std::set<uint64_t> pipelines;
size_t invalidPipelineDestroys = 0;
int pipelineFailure = 0;
std::vector<char> selectedShader;
VkDeviceSize alignment = 512;
bool allocationFails = false, mappingFails = false, bindingFails = false, memoryTypeUnavailable = false;
struct Seen
{
    Descriptor descriptor;
    std::vector<unsigned char> bytes;
    uint32_t x, y;
};
std::vector<Seen> submit(VkCommandBuffer cmd)
{
    std::vector<Seen> result;
    for (const auto& command : commands[id(cmd)])
    {
        const auto descriptor = descriptors.at(id(command.set));
        const auto& data = memory.at(buffers.at(id(descriptor.uniform.buffer)));
        const auto offset = descriptor.uniform.offset, range = descriptor.uniform.range;
        if (offset > data.size() || range > data.size() - offset)
            throw std::runtime_error("descriptor outside buffer");
        result.push_back(
            { descriptor, { data.begin() + offset, data.begin() + offset + range }, command.x, command.y });
    }
    return result;
}
void require(bool good, const char* message)
{
    if (!good)
        throw std::runtime_error(message);
}
struct Snapshot
{
    size_t calls = effects;
    decltype(memory) data = memory;
    void unchanged() const
    {
        require(calls == effects, "rejected call performed Vulkan/timing effects");
        require(data == memory, "rejected call wrote mapped constants");
    }
};
} // namespace boundary
using namespace boundary;

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* properties)
{
    *properties = {};
    properties->limits.minUniformBufferOffsetAlignment = alignment;
    properties->limits.maxUniformBufferRange = 65536;
    properties->limits.maxComputeWorkGroupCount[0] = properties->limits.maxComputeWorkGroupCount[1] = 65535;
}
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice, VkDeviceMemory mem, VkDeviceSize offset, VkDeviceSize,
                                           VkMemoryMapFlags, void** mapped)
{
    if (mappingFails)
        return VK_ERROR_MEMORY_MAP_FAILED;
    *mapped = memory.at(id(mem)).data() + offset;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice, VkDeviceMemory) {}
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice, VkBuffer buffer, const VkAllocationCallbacks*)
{
    buffers.erase(id(buffer));
    bufferSizes.erase(id(buffer));
}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice, VkDeviceMemory mem, const VkAllocationCallbacks*)
{
    memory.erase(id(mem));
}
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice, uint32_t count, const VkWriteDescriptorSet* writes,
                                                  uint32_t, const VkCopyDescriptorSet*)
{
    ++effects;
    for (uint32_t i = 0; i < count; ++i)
    {
        auto& descriptor = descriptors[id(writes[i].dstSet)];
        if (writes[i].dstBinding == 0)
            descriptor.uniform = *writes[i].pBufferInfo;
        if (writes[i].dstBinding == 1)
            descriptor.source = writes[i].pImageInfo->imageView;
        if (writes[i].dstBinding == 2)
            descriptor.target = writes[i].pImageInfo->imageView;
    }
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice, VkPipeline pipeline, const VkAllocationCallbacks*)
{
    if (pipeline != VK_NULL_HANDLE && pipelines.erase(id(pipeline)) != 1)
        ++invalidPipelineDestroys;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline pipeline)
{
    require(pipelines.count(id(pipeline)) == 1, "bound unowned pipeline");
    ++effects;
}
bool Shader_Vk::CreateComputePipeline(VkDevice, VkPipelineLayout, VkPipeline* pipeline, const std::vector<char>& code,
                                      const char* entry)
{
    require(std::string(entry) == "CSMain", "wrong pipeline entry point");
    selectedShader = code;
    if (pipelineFailure == 1)
        return false; // Module creation fails without touching output.
    if (pipelineFailure == 2)
    {
        *pipeline = VK_NULL_HANDLE;
        return false;
    }
    *pipeline = fresh<VkPipeline>();
    pipelines.insert(id(*pipeline));
    return pipelineFailure != 3; // Defensive helper-boundary robustness: owned non-null on false.
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint, VkPipelineLayout, uint32_t,
                                                   uint32_t, const VkDescriptorSet* sets, uint32_t, const uint32_t*)
{
    ++effects;
    bound[id(cmd)] = *sets;
}
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t)
{
    ++effects;
    commands[id(cmd)].push_back({ bound.at(id(cmd)), x, y });
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice, const VkBufferCreateInfo* info, const VkAllocationCallbacks*,
                                              VkBuffer* buffer)
{
    *buffer = fresh<VkBuffer>();
    buffers[id(*buffer)] = 0;
    bufferSizes[id(*buffer)] = info->size;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice, VkBuffer buffer, VkMemoryRequirements* requirements)
{
    *requirements = { bufferSizes.at(id(buffer)), 512, 1 };
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo* info,
                                                const VkAllocationCallbacks*, VkDeviceMemory* mem)
{
    if (allocationFails || info->memoryTypeIndex == UINT32_MAX)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *mem = fresh<VkDeviceMemory>();
    memory[id(*mem)].resize(info->allocationSize);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice, VkBuffer buffer, VkDeviceMemory mem, VkDeviceSize)
{
    if (bindingFails)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    buffers.at(id(buffer)) = id(mem);
    return VK_SUCCESS;
}
uint32_t Shader_Vk::FindMemoryType(VkPhysicalDevice, uint32_t, VkMemoryPropertyFlags)
{
    return memoryTypeUnavailable ? UINT32_MAX : 0;
}
bool Shader_Vk::CreateBufferResource(VkDevice device, VkPhysicalDevice physical, VkBuffer* buffer, VkDeviceMemory* mem,
                                     VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags)
{
    VkBufferCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &info, nullptr, buffer) != VK_SUCCESS)
        return false;
    VkMemoryRequirements requirements {};
    vkGetBufferMemoryRequirements(device, *buffer, &requirements);
    VkMemoryAllocateInfo alloc {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = FindMemoryType(physical, requirements.memoryTypeBits, flags);
    if (vkAllocateMemory(device, &alloc, nullptr, mem) != VK_SUCCESS)
    {
        vkDestroyBuffer(device, *buffer, nullptr);
        *buffer = VK_NULL_HANDLE;
        return false;
    }
    // Match the upstream helper's observed bug: successful allocation returns true even if binding fails.
    vkBindBufferMemory(device, *buffer, *mem, 0);
    return true;
}
void Shader_Vk::CreateConstantBuffer(VkDeviceSize size)
{
    if (CreateBufferResource(_device, _physicalDevice, &_constantBuffer, &_constantBufferMemory, size, 0, 0))
        vkMapMemory(_device, _constantBufferMemory, 0, size, 0, &_mappedConstantBuffer);
}
Shader_Vk::~Shader_Vk()
{
    vkDestroyPipeline(_device, _pipeline, nullptr);
    vkDestroyBuffer(_device, _constantBuffer, nullptr);
    vkFreeMemory(_device, _constantBufferMemory, nullptr);
}
// Only the upstream constructor's resource creation is replaced. Both baseline and fork
// receive the same valid pipeline, sampler and exactly three initially unused descriptor sets.
OS_Vk::OS_Vk(std::string name, VkDevice device, VkPhysicalDevice physical, bool up, Scaler scaler)
    : Shader_Vk(std::move(name), device, physical), _upsample(up), _scalerOverride(scaler)
{
    _pipeline = fresh<VkPipeline>();
    pipelines.insert(id(_pipeline));
    _pipelineLayout = fresh<VkPipelineLayout>();
    _textureSampler = fresh<VkSampler>();
    for (int i = 0; i < 3; ++i)
        _descriptorSets.push_back(fresh<VkDescriptorSet>());
    CreateConstantBuffer(scaler == Scaler::FSR1 ? sizeof(UpscaleShaderConstants) : sizeof(Constants));
    _init = true;
}
#if !TEST_RECORDING_SCALER
static Constants constants {};
static UpscaleShaderConstants fsr1Constants {};
struct ScopedGpuTime_Vk
{
    ScopedGpuTime_Vk(GpuTime_Vk*, VkCommandBuffer) { ++effects; }
};
struct Config
{
    struct Value
    {
        Scaler value_or_default() { return Scaler::Bicubic; }
    } OutputScalingDownscaler;
    static Config* Instance()
    {
        static Config value;
        return &value;
    }
};
struct Feature
{
    uint32_t TargetWidth() { return 1; }
    uint32_t TargetHeight() { return 1; }
    uint32_t DisplayWidth() { return 1; }
    uint32_t DisplayHeight() { return 1; }
};
struct State
{
    Feature feature;
    Feature* currentFeature = &feature;
    static State& Instance()
    {
        static State value;
        return value;
    }
};
#endif
// PRODUCTION_FUNCTIONS

#if TEST_RECORDING_SCALER
using Pass = DlssNr_Scaler_Vk;
#else
// Baseline has no reservation API. This adapter adds no protection and calls its actual
// Dispatch unchanged, so failures demonstrate old effects rather than absent symbols.
class Pass : public OS_Vk
{
  public:
    using OS_Vk::OS_Vk;
    struct Batch
    {
        uint32_t next = 0;
    };
    std::optional<Batch> Reserve(VkCommandBuffer, uint32_t) { return Batch {}; }
    bool Dispatch(VkCommandBuffer cmd, Batch&, const VkImageInfo& input, const VkImageInfo& output)
    {
        return OS_Vk::Dispatch(cmd, input, output);
    }
};
#endif
struct Fixture
{
    VkDevice device = fresh<VkDevice>();
    VkPhysicalDevice physical = fresh<VkPhysicalDevice>();
    VkCommandPool pool = fresh<VkCommandPool>();
    std::vector<uint64_t> owned;
    VkCommandBuffer begin()
    {
        auto cmd = fresh<VkCommandBuffer>();
        owned.push_back(id(cmd));
        auto& registry = DlssNr::VkRecording::Recordings();
        registry.Allocate(id(device), id(pool), { id(cmd) });
        registry.Begin(id(cmd));
        return cmd;
    }
    ~Fixture()
    {
        DlssNr::VkRecording::Recordings().ForgetDevice(id(device));
        for (auto cmd : owned)
            commands.erase(cmd);
    }
};
VkImageInfo resource(uint32_t w, uint32_t h)
{
    VkImageInfo result {};
    result.Image = fresh<VkImage>();
    result.ImageView = fresh<VkImageView>();
    result.Width = w;
    result.Height = h;
    result.Format = VK_FORMAT_R16G16B16A16_SFLOAT;
    return result;
}
void checkDimensions(const Seen& seen, int32_t sw, int32_t sh, int32_t dw, int32_t dh)
{
    int32_t dimensions[4] {};
    require(seen.bytes.size() >= sizeof(dimensions), "short constants");
    std::memcpy(dimensions, seen.bytes.data(), sizeof(dimensions));
    require(dimensions[0] == sw && dimensions[1] == sh && dimensions[2] == dw && dimensions[3] == dh,
            "recorded scaler constants changed before execution/resubmission");
}
void immutableRecordings()
{
    Fixture f;
    Pass pass("test", f.device, f.physical, false, Scaler::Bicubic);
    auto a = f.begin(), b = f.begin();
    auto inA = resource(1920, 1080), outA = resource(1280, 720);
    auto inB = resource(2560, 1440), outB = resource(960, 540);
    {
        auto batch = pass.Reserve(a, 1);
        require(bool(batch), "first reserve");
        require(pass.Dispatch(a, *batch, inA, outA), "first dispatch");
    }
    DlssNr::VkRecording::Recordings().End(id(a));
    auto first = submit(a);
    checkDimensions(first.at(0), 1920, 1080, 1280, 720);
    {
        auto batch = pass.Reserve(b, 1);
        require(bool(batch), "second reserve");
        require(pass.Dispatch(b, *batch, inB, outB), "second dispatch");
    }
    auto again = submit(a);
    checkDimensions(again.at(0), 1920, 1080, 1280, 720);
    require(again.at(0).descriptor.source == inA.ImageView && again.at(0).descriptor.target == outA.ImageView,
            "recorded image descriptors changed");
    auto second = submit(b);
    checkDimensions(second.at(0), 2560, 1440, 960, 540);
    require(first.at(0).descriptor.uniform.offset != second.at(0).descriptor.uniform.offset,
            "live records share uniform region");
    require(second.at(0).descriptor.uniform.offset % 512 == 0, "slot offset violates device alignment");
    require(second.at(0).descriptor.uniform.range == sizeof(Constants), "incorrect ordinary payload range");
}
void boundedRetention()
{
    Fixture f;
    Pass pass("test", f.device, f.physical, false, Scaler::Bicubic);
    auto a = f.begin(), b = f.begin();
    auto in = resource(640, 480), out = resource(320, 240);
    {
        auto batch = pass.Reserve(a, 3);
        require(bool(batch), "three-slot reserve");
        for (int i = 0; i < 3; ++i)
            require(pass.Dispatch(a, *batch, in, out), "dispatch reserved slot");
        Snapshot effectsBefore;
        require(!pass.Dispatch(a, *batch, in, out), "exhausted batch admitted dispatch");
        effectsBefore.unchanged();
    }
    DlssNr::VkRecording::Recordings().End(id(a));
    for (int submitIndex = 0; submitIndex < 2; ++submitIndex)
    {
        require(submit(a).size() == 3, "executable recording cannot be resubmitted");
        Snapshot before;
        require(!pass.Reserve(b, 1), "ended/submitted recording released live slots");
        before.unchanged();
    }
    DlssNr::VkRecording::Recordings().Reset(id(a));
    require(bool(pass.Reserve(b, 3)), "reset failed to release slots");
}
void admission()
{
    Fixture f;
    Pass pass("test", f.device, f.physical, false, Scaler::Bicubic),
        other("other", f.device, f.physical, false, Scaler::Bicubic);
    auto a = f.begin(), b = f.begin();
    auto in = resource(640, 480), out = resource(320, 240);
    auto batch = pass.Reserve(a, 1);
    require(bool(batch), "reserve");
    const auto rejected = [&](auto&& operation)
    {
        Snapshot before;
        require(!operation(), "invalid dispatch admitted");
        before.unchanged();
    };
    rejected([&] { return other.Dispatch(a, *batch, in, out); });
    rejected([&] { return pass.Dispatch(b, *batch, in, out); });
    auto noView = in;
    noView.ImageView = VK_NULL_HANDLE;
    rejected([&] { return pass.Dispatch(a, *batch, noView, out); });
    auto noImage = out;
    noImage.Image = VK_NULL_HANDLE;
    rejected([&] { return pass.Dispatch(a, *batch, in, noImage); });
    for (int i = 0; i < 4; ++i)
    {
        auto badIn = in, badOut = out;
        (i == 0 ? badIn.Width : i == 1 ? badIn.Height : i == 2 ? badOut.Width : badOut.Height) = 0;
        rejected([&] { return pass.Dispatch(a, *batch, badIn, badOut); });
    }
    auto huge = in;
    huge.Width = 0xffffffffu;
    rejected([&] { return pass.Dispatch(a, *batch, huge, out); });
    auto tooManyGroups = out;
    tooManyGroups.Width = 524281;
    rejected([&] { return pass.Dispatch(a, *batch, in, tooManyGroups); });
    auto moved = std::move(*batch);
    rejected([&] { return pass.Dispatch(a, *batch, in, out); });
    require(pass.Dispatch(a, moved, in, out), "invalid calls consumed slot");
}
void staleAndWrongDevice()
{
    Fixture f;
    Pass pass("test", f.device, f.physical, false, Scaler::Bicubic);
    auto a = f.begin();
    auto batch = pass.Reserve(a, 1);
    require(bool(batch), "reserve");
    auto in = resource(640, 480), out = resource(320, 240);
    DlssNr::VkRecording::Recordings().Reset(id(a));
    DlssNr::VkRecording::Recordings().Begin(id(a));
    Snapshot before;
    require(!pass.Dispatch(a, *batch, in, out), "stale batch admitted after rebegin");
    before.unchanged();
    auto otherDevice = fresh<VkDevice>();
    auto foreign = fresh<VkCommandBuffer>();
    DlssNr::VkRecording::Recordings().Allocate(id(otherDevice), id(f.pool), { id(foreign) });
    DlssNr::VkRecording::Recordings().Begin(id(foreign));
    require(!pass.Reserve(foreign, 1), "foreign-device command buffer admitted");
    DlssNr::VkRecording::Recordings().ForgetDevice(id(otherDevice));
}
void fsrPayload()
{
    Fixture f;
    Pass pass("test", f.device, f.physical, true, Scaler::FSR1);
    auto a = f.begin();
    auto batch = pass.Reserve(a, 2);
    require(bool(batch), "FSR reserve");
    auto input = resource(640, 480), output = resource(1280, 960);
    require(pass.Dispatch(a, *batch, input, output), "FSR dispatch");
    auto first = submit(a).at(0);
    require(first.descriptor.uniform.range == sizeof(UpscaleShaderConstants),
            "FSR descriptor range differs from active payload");
    // EASU con0 = {input/output x, input/output y, .5*input/output x-.5, ditto y}.
    const uint32_t expected[4] = { 0x3f000000, 0x3f000000, 0xbe800000, 0xbe800000 };
    require(first.bytes.size() >= sizeof(expected) && std::memcmp(first.bytes.data(), expected, sizeof(expected)) == 0,
            "FSR constants differ from hand-derived EASU con0");
    require(first.x == 80 && first.y == 60, "FSR thread groups");
    auto output2 = resource(1920, 1440);
    require(pass.Dispatch(a, *batch, input, output2), "second FSR dispatch");
    auto second = submit(a);
    require(second.at(0).bytes == first.bytes, "FSR live payload overwritten");
    require(second.at(1).descriptor.uniform.offset % 512 == 0 &&
                second.at(1).descriptor.uniform.offset != first.descriptor.uniform.offset,
            "FSR slot alignment/separation");
}
void failedConstruction()
{
    Fixture f;
    auto cmd = f.begin();
    allocationFails = true;
    {
        Pass pass("allocation failure", f.device, f.physical, false, Scaler::Bicubic);
        allocationFails = false;
        Snapshot before;
        require(!pass.Reserve(cmd, 1), "failed allocation admitted reservation");
        before.unchanged();
    }
    for (auto invalid : { Scaler::Count, static_cast<Scaler>(0xffffffffu) })
    {
        Pass pass("invalid scaler", f.device, f.physical, false, invalid);
        Snapshot before;
        require(!pass.Reserve(cmd, 1), "invalid scaler admitted reservation");
        before.unchanged();
    }
    mappingFails = true;
    {
        Pass pass("mapping failure", f.device, f.physical, false, Scaler::Bicubic);
        mappingFails = false;
        Snapshot before;
        require(!pass.Reserve(cmd, 1), "failed mapping admitted reservation");
        before.unchanged();
    }
}
void bindingFailure()
{
    Fixture f;
    auto cmd = f.begin();
    const auto initialBuffers = buffers.size(), initialMemory = memory.size();
    bindingFails = true;
    {
        Pass pass("binding failure", f.device, f.physical, false, Scaler::Bicubic);
        bindingFails = false;
        Snapshot before;
        require(!pass.Reserve(cmd, 1), "unbound constant buffer admitted reservation");
        before.unchanged();
        require(buffers.size() == initialBuffers && memory.size() == initialMemory,
                "failed candidate resources retained until destruction");
    }
    memoryTypeUnavailable = true;
    {
        Pass pass("no matching memory type", f.device, f.physical, false, Scaler::Bicubic);
        memoryTypeUnavailable = false;
        Snapshot before;
        require(!pass.Reserve(cmd, 1), "missing memory type admitted reservation");
        before.unchanged();
        require(buffers.size() == initialBuffers && memory.size() == initialMemory,
                "missing-memory-type candidate resources leaked");
    }
}
void wrongOutputFormat()
{
    Fixture f;
    Pass pass("format", f.device, f.physical, true, Scaler::Lanczos3);
    auto cmd = f.begin();
    auto batch = pass.Reserve(cmd, 1);
    require(bool(batch), "format reserve");
    auto input = resource(640, 480), output = resource(800, 600);
    for (auto format : { VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT })
    {
        auto wrong = output;
        wrong.Format = format;
        Snapshot before;
        require(!pass.Dispatch(cmd, *batch, input, wrong), "incompatible storage output format admitted");
        before.unchanged();
    }
    require(pass.Dispatch(cmd, *batch, input, output), "format rejection consumed reservation");
}
void replacementPipelineFailure()
{
    Fixture f;
    auto cmd = f.begin();
    const auto initial = pipelines.size();
    const auto badDestroys = invalidPipelineDestroys;
    for (int failure : { 1, 2, 3 })
    {
        pipelineFailure = failure;
        {
            Pass pass("pipeline failure", f.device, f.physical, true, Scaler::Lanczos3);
            pipelineFailure = 0;
            Snapshot before;
            require(!pass.Reserve(cmd, 1), "replacement failure admitted upstream pipeline fallback");
            before.unchanged();
            require(pipelines.size() == initial + 1, "failed replacement leaked candidate or lost base ownership");
        }
        require(pipelines.size() == initial, "pipeline destruction leaked owned handle");
        require(invalidPipelineDestroys == badDestroys, "destroyed unowned pipeline output");
    }
}
void selectedPipelineBytes()
{
#if TEST_NR_RGBA_SHADERS
    Fixture f;
    const auto initial = pipelines.size();
    const std::vector<std::vector<char>> down = { { std::begin(nr_bcds_bicubic_spv), std::end(nr_bcds_bicubic_spv) },
                                                  { std::begin(nr_bcds_catmull_spv), std::end(nr_bcds_catmull_spv) },
                                                  { std::begin(nr_bcds_lanczos2_spv), std::end(nr_bcds_lanczos2_spv) },
                                                  { std::begin(nr_bcds_lanczos3_spv), std::end(nr_bcds_lanczos3_spv) },
                                                  { std::begin(nr_bcds_kaiser2_spv), std::end(nr_bcds_kaiser2_spv) },
                                                  { std::begin(nr_bcds_kaiser3_spv), std::end(nr_bcds_kaiser3_spv) },
                                                  { std::begin(nr_bcds_magc_spv), std::end(nr_bcds_magc_spv) },
                                                  { std::begin(nr_fsr_easu_spv), std::end(nr_fsr_easu_spv) } };
    for (bool up : { false, true })
        for (uint32_t filter = 0; filter < 8; ++filter)
        {
            selectedShader.clear();
            {
                Pass pass("selection", f.device, f.physical, up, static_cast<Scaler>(filter));
                const auto expected = up && filter != 7
                                          ? std::vector<char>(std::begin(nr_bcus_spv), std::end(nr_bcus_spv))
                                          : down[filter];
                require(selectedShader == expected, "selected pipeline bytes differ from requested NR filter");
                require(pass.CanRender(), "replacement pipeline not renderable");
                require(pipelines.size() == initial + 1, "successful replacement retained base pipeline");
            }
            require(pipelines.size() == initial, "successful replacement pipeline leaked");
        }
#else
    throw std::runtime_error("generated NR shader header needed for selected-byte contract");
#endif
}
int main()
{
    int failed = 0;
    for (const auto& test : std::vector<std::pair<const char*, void (*)()>> {
             { "immutable recordings and executable resubmission", immutableRecordings },
             { "bounded retention until observed reset", boundedRetention },
             { "invalid dispatch has no side effects", admission },
             { "stale ticket and wrong device rejected", staleAndWrongDevice },
             { "FSR payload range, alignment and immutability", fsrPayload },
             { "failed construction rejects reservation", failedConstruction },
             { "binding failure rejects and reclaims candidate", bindingFailure },
             { "incompatible output format has no effects", wrongOutputFormat },
             { "replacement failure rejects and cleans owned candidate", replacementPipelineFailure },
             { "all requested filters select exact embedded NR bytes", selectedPipelineBytes } })
    {
        try
        {
            test.second();
            std::cout << "PASS: " << test.first << '\n';
        }
        catch (const std::exception& error)
        {
            ++failed;
            std::cerr << "FAIL: " << test.first << ": " << error.what() << '\n';
        }
    }
    return failed ? 1 : 0;
}
