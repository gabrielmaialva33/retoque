#pragma once

#include "DlssNr_VkPreUpscale.h"
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

// Passive facts, never GPU commands or permission to execute NR. Vulkan adapters translate handles to integers.
// A snapshot is evidence from intercepted calls, not proof that all API entrypoints were intercepted.
namespace DlssNr::VkPre
{
using Handle = uint64_t;
struct ObjectKey
{
    Handle device = 0, handle = 0;
    bool operator==(const ObjectKey&) const = default;
};
struct ObjectHash
{
    size_t operator()(ObjectKey key) const noexcept
    {
        return std::hash<Handle> {}(key.device) ^ (std::hash<Handle> {}(key.handle) << 1);
    }
};
template <class T> using ObjectMap = std::unordered_map<ObjectKey, T, ObjectHash>;
struct Range
{
    uint32_t aspect = 0, mip = 0, levels = 0, layer = 0, layers = 0;
    bool operator==(const Range&) const = default;
};
struct ImageFact
{
    Handle device = 0;
    uint32_t width = 0, height = 0, depth = 0, levels = 0, layers = 0;
    uint32_t format = 0, usage = 0, samples = 0, type = 0;
    bool simple = true, concurrent = false;
    std::vector<uint32_t> families;
    uint64_t incarnation = 0;
};
struct ViewFact
{
    Handle device = 0, image = 0;
    uint32_t format = 0, type = 0;
    Range range;
    bool simple = true;
    uint64_t imageIncarnation = 0, incarnation = 0;
    bool operator==(const ViewFact&) const = default;
};
struct FeatureIdentity
{
    Handle device = 0, handle = 0;
    Feature kind = Feature::Unknown;
    uint64_t incarnation = 0, deviceIncarnation = 0;
    Handle source = 0; // The game's NGX handle pointer, retained as identity only, never dereferenced.
    bool operator==(const FeatureIdentity&) const = default;
};
struct DescriptorBinding
{
    uint32_t point = 0, first = 0;
    Handle layout = 0;
    std::vector<Handle> sets;
    std::vector<uint32_t> offsets;
};
struct PushBinding
{
    Handle layout = 0;
    uint32_t stages = 0, offset = 0;
    std::vector<uint8_t> bytes;
};
struct LayoutFact
{
    Range range;
    uint32_t layout = 0;
    uint64_t imageIncarnation = 0;
};
struct CommandFact
{
    Handle device = 0, pool = 0;
    uint32_t family = ~0u;
    std::optional<Ticket> ticket;
    bool primary = false, recording = false, oneTime = false, simultaneous = false;
    bool inside = false, renderingKnown = true, bindingsKnown = true;
    std::array<Handle, 2> pipelines {};
    std::vector<DescriptorBinding> descriptors;
    std::vector<PushBinding> push;
    ObjectMap<LayoutFact> layouts;
};
class Observations
{
    struct PoolFact
    {
        Handle device;
        uint32_t family;
    };
    mutable std::mutex _mutex;
    uint64_t _serial = 0;
    ObjectMap<uint64_t> _devices;
    ObjectMap<FeatureIdentity> _features;
    ObjectMap<ImageFact> _images;
    ObjectMap<ViewFact> _views;
    ObjectMap<PoolFact> _pools;
    ObjectMap<CommandFact> _commands;
    // Bounded passive storage. Exhaustion is an observation failure, never permission to guess.
    static constexpr size_t kLimit = 16384;
    uint64_t Next()
    {
        if (_serial == std::numeric_limits<uint64_t>::max())
            return 0;
        return ++_serial;
    }
    // Command-only Vulkan callbacks have no device argument. A dispatchable handle normally identifies one
    // device; ambiguous observations are refused, never resolved by first/last insertion.
    template <class Map> static auto Unique(Map& map, Handle handle)
    {
        auto found = map.end();
        for (auto it = map.begin(); it != map.end(); ++it)
        {
            if (it->first.handle != handle)
                continue;
            if (found != map.end())
                return map.end();
            found = it;
        }
        return found;
    }
    CommandFact* Recording(Handle command)
    {
        auto it = Unique(_commands, command);
        return it != _commands.end() && it->second.recording ? &it->second : nullptr;
    }
    static void Reset(CommandFact& c)
    {
        auto device = c.device, pool = c.pool;
        auto family = c.family;
        bool primary = c.primary;
        c = {};
        c.device = device;
        c.pool = pool;
        c.family = family;
        c.primary = primary;
    }
    void EraseDevice(Handle device)
    {
        _devices.erase({ device, device });
        std::erase_if(_features, [=](const auto& p) { return p.second.device == device; });
        std::erase_if(_images, [=](const auto& p) { return p.second.device == device; });
        std::erase_if(_views, [=](const auto& p) { return p.second.device == device; });
        std::erase_if(_pools, [=](const auto& p) { return p.second.device == device; });
        std::erase_if(_commands, [=](const auto& p) { return p.second.device == device; });
    }

  public:
    void Clear()
    {
        std::lock_guard lock(_mutex);
        _devices.clear();
        _features.clear();
        _images.clear();
        _views.clear();
        _pools.clear();
        _commands.clear();
        // Never reuse incarnation numbers after a blind interval.
    }
    void Device(Handle device)
    {
        std::lock_guard lock(_mutex);
        EraseDevice(device);
        if (device && _devices.size() < kLimit)
            _devices[{ device, device }] = Next();
    }
    void DestroyDevice(Handle device)
    {
        std::lock_guard lock(_mutex);
        EraseDevice(device);
    }
    void FeatureCreated(Handle device, Handle handle, Feature kind, Handle source = 0)
    {
        std::lock_guard lock(_mutex);
        _features.erase({ device, handle });
        auto d = _devices.find({ device, device });
        if (handle && d != _devices.end() && d->second && _features.size() < kLimit)
            _features[{ device, handle }] = { device, handle, kind, Next(), d->second, source };
    }
    void ForgetFeature(Handle device, Handle handle)
    {
        std::lock_guard lock(_mutex);
        _features.erase({ device, handle });
    }
    std::optional<FeatureIdentity> FindFeature(Handle device, Handle handle) const
    {
        std::lock_guard lock(_mutex);
        auto it = _features.find({ device, handle });
        if (it == _features.end() || !it->second.incarnation)
            return {};
        return it->second;
    }
    void ForgetFeatureSource(Handle source, Handle handle)
    {
        if (!source)
            return;
        std::lock_guard lock(_mutex);
        std::erase_if(_features, [=](const auto& p) { return p.second.source == source && p.first.handle == handle; });
    }
    bool Current(const FeatureIdentity& identity) const
    {
        auto current = FindFeature(identity.device, identity.handle);
        return current && *current == identity;
    }
    void Image(Handle image, ImageFact fact)
    {
        std::lock_guard lock(_mutex);
        _images.erase({ fact.device, image });
        if (!image || !_devices.count({ fact.device, fact.device }) || _images.size() >= kLimit)
            return;
        fact.incarnation = Next();
        _images[{ fact.device, image }] = std::move(fact);
    }
    void DestroyImage(Handle device, Handle image)
    {
        std::lock_guard lock(_mutex);
        _images.erase({ device, image });
    }
    void View(Handle view, ViewFact fact)
    {
        std::lock_guard lock(_mutex);
        _views.erase({ fact.device, view });
        auto i = _images.find({ fact.device, fact.image });
        if (!view || i == _images.end() || i->second.device != fact.device || _views.size() >= kLimit)
            return;
        fact.imageIncarnation = i->second.incarnation;
        fact.incarnation = Next();
        if (fact.incarnation)
            _views[{ fact.device, view }] = fact;
    }
    void DestroyView(Handle device, Handle view)
    {
        std::lock_guard lock(_mutex);
        _views.erase({ device, view });
    }
    std::optional<ImageFact> ImageForView(Handle device, Handle view) const
    {
        std::lock_guard lock(_mutex);
        auto v = _views.find({ device, view });
        if (v == _views.end())
            return {};
        auto i = _images.find({ v->second.device, v->second.image });
        if (i == _images.end() || i->second.incarnation != v->second.imageIncarnation)
            return {};
        return i->second;
    }
    std::optional<ViewFact> ViewInfo(Handle device, Handle view) const
    {
        std::lock_guard lock(_mutex);
        auto it = _views.find({ device, view });
        if (it == _views.end())
            return {};
        return it->second;
    }
    bool CurrentView(Handle view, const ViewFact& identity) const
    {
        auto current = ViewInfo(identity.device, view);
        return current && current->incarnation == identity.incarnation &&
               current->imageIncarnation == identity.imageIncarnation &&
               ImageForView(identity.device, view).has_value();
    }
    std::optional<Handle> DeviceOfCommand(Handle buffer) const
    {
        std::lock_guard lock(_mutex);
        auto it = Unique(_commands, buffer);
        if (it == _commands.end())
            return {};
        return it->first.device;
    }
    void Pool(Handle device, Handle pool, uint32_t family)
    {
        std::lock_guard lock(_mutex);
        std::erase_if(_commands, [=](const auto& p) { return p.first.device == device && p.second.pool == pool; });
        _pools.erase({ device, pool });
        if (pool && _devices.count({ device, device }) && _pools.size() < kLimit)
            _pools[{ device, pool }] = { device, family };
    }
    void Allocate(Handle device, Handle pool, Handle buffer, bool primary)
    {
        std::lock_guard lock(_mutex);
        _commands.erase({ device, buffer });
        auto p = _pools.find({ device, pool });
        if (!buffer || p == _pools.end() || p->second.device != device || _commands.size() >= kLimit)
            return;
        CommandFact c;
        c.device = device;
        c.pool = pool;
        c.family = p->second.family;
        c.primary = primary;
        _commands[{ device, buffer }] = std::move(c);
    }
    // before/after are snapshots around this successful native Begin, not a later arbitrary Current.
    void Begin(Handle buffer, bool success, bool oneTime, bool simultaneous, std::optional<Ticket> before = {},
               std::optional<Ticket> after = {})
    {
        if (!success)
            return;
        std::lock_guard lock(_mutex);
        auto it = Unique(_commands, buffer);
        if (it == _commands.end())
            return;
        auto& c = it->second;
        Reset(c);
        c.recording = true;
        const auto reuse = simultaneous ? VkRecording::Reuse::Simultaneous : VkRecording::Reuse::Serial;
        if (after && ValidTicket(*after) && after->device == c.device && after->buffer == buffer &&
            after->reuse == reuse && (!before || !SameTicket(*before, *after)))
            c.ticket = after;
        c.oneTime = oneTime;
        c.simultaneous = simultaneous;
        c.renderingKnown = c.primary; // Secondary inheritance is deliberately unqualified.
    }
    std::optional<Ticket> Associated(Handle device, Handle buffer, std::optional<Ticket> current) const
    {
        std::lock_guard lock(_mutex);
        auto c = _commands.find({ device, buffer });
        if (!current || c == _commands.end() || !c->second.recording || !c->second.ticket)
            return {};
        const auto observed = *c->second.ticket;
        if (!SameTicket(observed, *current) || observed.reuse != current->reuse)
            return {};
        return observed;
    }
    void End(Handle buffer, bool success)
    {
        if (!success)
            return;
        std::lock_guard lock(_mutex);
        if (auto* c = Recording(buffer))
            c->recording = false;
    }
    void ResetBuffer(Handle buffer, bool success)
    {
        if (!success)
            return;
        std::lock_guard lock(_mutex);
        auto it = Unique(_commands, buffer);
        if (it != _commands.end())
            Reset(it->second);
    }
    void Free(Handle device, Handle buffer)
    {
        std::lock_guard lock(_mutex);
        _commands.erase({ device, buffer });
    }
    void ResetPool(Handle device, Handle pool, bool success)
    {
        if (!success)
            return;
        std::lock_guard lock(_mutex);
        for (auto& [h, c] : _commands)
            if (h.device == device && c.pool == pool)
                Reset(c);
    }
    void DestroyPool(Handle device, Handle pool)
    {
        std::lock_guard lock(_mutex);
        _pools.erase({ device, pool });
        std::erase_if(_commands, [=](const auto& p) { return p.first.device == device && p.second.pool == pool; });
    }
    std::optional<CommandFact> Command(Handle device, Handle buffer) const
    {
        std::lock_guard lock(_mutex);
        auto it = _commands.find({ device, buffer });
        if (it == _commands.end())
            return {};
        return it->second;
    }
    std::optional<CommandFact> Command(Handle buffer) const
    {
        std::lock_guard lock(_mutex);
        auto it = Unique(_commands, buffer);
        if (it == _commands.end())
            return {};
        return it->second;
    }
    void Barrier(Handle buffer, Handle image, Range range, uint32_t oldLayout, uint32_t newLayout,
                 uint32_t sourceFamily, uint32_t targetFamily)
    {
        std::lock_guard lock(_mutex);
        auto* c = Recording(buffer);
        if (!c)
            return;
        auto i = _images.find({ c->device, image });
        auto prior = c->layouts.find({ c->device, image });
        bool conflict = prior != c->layouts.end() && oldLayout != 0 &&
                        (prior->second.range != range || prior->second.layout != oldLayout ||
                         prior->second.imageIncarnation != (i == _images.end() ? 0 : i->second.incarnation));
        c->layouts.erase({ c->device, image });
        // An explicit whole tracked range only; ownership transfer observations do not prove a matching release.
        if (i == _images.end() || i->second.device != c->device || conflict || sourceFamily != ~0u ||
            targetFamily != ~0u || !range.aspect || !range.levels || !range.layers || range.mip >= i->second.levels ||
            range.levels > i->second.levels - range.mip || range.layer >= i->second.layers ||
            range.layers > i->second.layers - range.layer || newLayout == 0 || c->layouts.size() >= kLimit)
            return;
        c->layouts[{ c->device, image }] = { range, newLayout, i->second.incarnation };
    }
    std::optional<uint32_t> Layout(Handle buffer, Handle view) const
    {
        std::lock_guard lock(_mutex);
        auto c = Unique(_commands, buffer);
        if (c == _commands.end())
            return {};
        auto v = _views.find({ c->first.device, view });
        if (c == _commands.end() || !c->second.recording || c->second.inside || !c->second.renderingKnown ||
            v == _views.end())
            return {};
        auto i = _images.find({ v->second.device, v->second.image });
        if (i == _images.end() || i->second.device != c->second.device ||
            i->second.incarnation != v->second.imageIncarnation)
            return {};
        auto l = c->second.layouts.find({ c->first.device, v->second.image });
        if (l == c->second.layouts.end() || l->second.range != v->second.range ||
            l->second.imageIncarnation != i->second.incarnation)
            return {};
        return l->second.layout;
    }
    void Rendering(Handle buffer, bool begin, bool traditional)
    {
        std::lock_guard lock(_mutex);
        if (auto* c = Recording(buffer))
        {
            if (c->inside == begin)
                c->renderingKnown = false;
            c->inside = begin;
            if (traditional)
                c->layouts.clear(); // implicit attachment transitions are not reconstructed
        }
    }
    void Unsupported(Handle buffer)
    {
        std::lock_guard lock(_mutex);
        if (auto* c = Recording(buffer))
            c->bindingsKnown = false;
    }
    void ExecuteSecondary(Handle buffer)
    {
        std::lock_guard lock(_mutex);
        if (auto* c = Recording(buffer))
        {
            c->bindingsKnown = false;
            c->renderingKnown = false;
            c->layouts.clear();
        }
    }
    void Pipeline(Handle buffer, uint32_t point, Handle pipeline)
    {
        std::lock_guard lock(_mutex);
        if (auto* c = Recording(buffer))
        {
            if (point < 2)
                c->pipelines[point] = pipeline;
            else
                c->bindingsKnown = false;
        }
    }
    void Descriptors(Handle buffer, uint32_t point, Handle layout, uint32_t first, uint32_t count, const Handle* sets,
                     uint32_t offsetCount, const uint32_t* offsets)
    {
        std::lock_guard lock(_mutex);
        auto* c = Recording(buffer);
        if (!c)
            return;
        if (point >= 2 || count > 32 || first > 32 - count || offsetCount > 256 || (count && !sets) ||
            (offsetCount && !offsets) || c->descriptors.size() >= 64)
        {
            c->bindingsKnown = false;
            return;
        }
        DescriptorBinding b;
        b.point = point;
        b.first = first;
        b.layout = layout;
        if (count)
            b.sets.assign(sets, sets + count);
        if (offsetCount)
            b.offsets.assign(offsets, offsets + offsetCount);
        c->descriptors.push_back(std::move(b)); // ordered replay, including layout incompatibility effects
    }
    void Push(Handle buffer, Handle layout, uint32_t stages, uint32_t offset, uint32_t size, const void* data)
    {
        std::lock_guard lock(_mutex);
        auto* c = Recording(buffer);
        if (!c)
            return;
        if (size > 256 || offset > 256 - size || (size && !data) || c->push.size() >= 64)
        {
            c->bindingsKnown = false;
            return;
        }
        PushBinding b;
        b.layout = layout;
        b.stages = stages;
        b.offset = offset;
        b.bytes.resize(size);
        if (size)
            std::memcpy(b.bytes.data(), data, size);
        c->push.push_back(std::move(b));
    }
};
} // namespace DlssNr::VkPre
