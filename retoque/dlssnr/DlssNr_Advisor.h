#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace DlssNr::Advisor
{
enum class State
{
    Idle,
    Settling,
    Sampling,
    Restoring,
    Done,
    Aborted
};
enum class Abort
{
    Cancelled,
    MenuClosed,
    Disabled,
    TimingOff,
    RouteChanged,
    Resized,
    DeviceLost,
    Unsupported
};
struct Settings
{
    float scale = 1;
    uint32_t cadence = 1;
    bool operator==(const Settings&) const = default;
};
struct Context
{
    uintptr_t device = 0;
    uint32_t route = 0, width = 0, height = 0, renderWidth = 0, renderHeight = 0, passes = 1;
    Settings original {};
    bool afterRR = false;
    bool operator==(const Context&) const = default;
};
struct Sample
{
    uint64_t token = 0, id = 0, contract = 0;
    double gpuMs = 0, frameMs = 0;
    bool confirmed = false, reset = false, capture = false, carried = false;
};
struct Result
{
    Settings settings {};
    double medianMs = 0, worst5Ms = 0, share = 0, frameMs = 0;
    double meanMs = 0, meanFrameMs = 0, frameWorst5Ms = 0;
    size_t modelCount = 0, carriedCount = 0;
    size_t count = 0;
    uint64_t contract = 0;
    std::string reason;
};
inline std::vector<Settings> Candidates(Settings original)
{
    std::vector<Settings> result { original };
    for (float scale : { 0.75f, 0.5f })
        if (scale < original.scale)
            result.push_back({ scale, original.cadence });
    if (original.cadence < 2)
        result.push_back({ original.scale, 2 });
    if (original.scale > 0.25f)
        result.push_back({ 0.25f, original.cadence });
    if (original.cadence < 4)
        result.push_back({ original.scale, 4 });
    return result;
}
inline double Median(std::vector<double> values)
{
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}
inline double Mean(const std::vector<double>& values)
{
    double result = 0;
    for (double value : values)
        result += value;
    return values.empty() ? 0 : result / values.size();
}
inline double SlowTail(std::vector<double> values)
{
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end(), std::greater<double>());
    values.resize((values.size() + 19) / 20);
    return Mean(values);
}
inline Result Summarize(Settings settings, const std::vector<Sample>& samples)
{
    Result result;
    result.settings = settings;
    result.count = samples.size();
    if (samples.empty())
        return result;
    result.contract = samples.front().contract;
    std::vector<double> gpu, frame;
    for (const auto& sample : samples)
    {
        gpu.push_back(sample.gpuMs);
        frame.push_back(sample.frameMs);
        sample.carried ? ++result.carriedCount : ++result.modelCount;
    }
    result.medianMs = Median(gpu);
    result.frameMs = Median(frame);
    result.meanMs = Mean(gpu);
    result.meanFrameMs = Mean(frame);
    result.worst5Ms = SlowTail(gpu);
    result.frameWorst5Ms = SlowTail(frame);
    result.share = 100 * result.meanMs / result.meanFrameMs;
    return result;
}
// A target is met only by the whole window's mean, with a slow-tail cap of 1.5 frame budgets.
// At least the scheduled model-frame share must have been observed; carried-only gaps cannot win.
inline constexpr double TailBudgetMultiple = 1.5;
inline std::optional<size_t> Recommend(const std::vector<Result>& results, double targetFps)
{
    if (!std::isfinite(targetFps) || targetFps < 1 || targetFps > 1000)
        return {};
    for (size_t i = 0; i < results.size(); ++i)
    {
        const auto& result = results[i];
        if (result.reason.empty() && result.count == 60 && result.settings.cadence >= 1 &&
            result.settings.cadence <= 4 &&
            result.modelCount >= (60 + result.settings.cadence - 1) / result.settings.cadence &&
            std::isfinite(result.meanFrameMs) && result.meanFrameMs > 0 && std::isfinite(result.frameWorst5Ms) &&
            result.frameWorst5Ms > 0 && result.meanFrameMs <= 1000 / targetFps &&
            result.frameWorst5Ms <= TailBudgetMultiple * 1000 / targetFps)
            return i;
    }
    return {};
}
class Session
{
    State state = State::Idle;
    Settings original {};
    std::vector<Settings> candidates;
    std::vector<Result> results;
    std::vector<Sample> samples;
    std::vector<uint64_t> seen;
    std::optional<Context> context;
    size_t current = 0, settled = 0;
    uint64_t serial = 0, token = 0, contract = 0, settledThrough = 0;
    double began = 0, settleBegan = 0, target = 60;
    std::string refusal;
    Abort abort = Abort::Cancelled;
    bool aborted = false, resultValid = false;
    void Next(double now)
    {
        ++current;
        samples.clear();
        seen.clear();
        settled = 0;
        contract = settledThrough = 0;
        refusal.clear();
        if (current == candidates.size())
        {
            state = State::Restoring;
            token = 0;
            return;
        }
        token = ++serial;
        began = settleBegan = now;
        state = State::Settling;
    }

  public:
    static constexpr size_t SettleCount = 30, SampleCount = 60;
    static constexpr double SettleMs = 1000, TimeoutMs = 20000;
    bool Active() const { return state == State::Settling || state == State::Sampling; }
    State Status() const { return state; }
    uint64_t Token() const { return token; }
    size_t Index() const { return current; }
    size_t Total() const { return candidates.size(); }
    size_t Count() const { return state == State::Settling ? settled : samples.size(); }
    Abort AbortReason() const { return abort; }
    const std::vector<Result>& Results() const { return results; }
    const std::optional<Context>& BoundContext() const { return context; }
    double Target() const { return target; }
    bool ResultValid() const { return resultValid; }
    void InvalidateResult() { resultValid = false; }
    std::optional<size_t> Recommendation() const
    {
        return state == State::Done ? Recommend(results, target) : std::nullopt;
    }
    std::optional<Settings> Override() const { return Active() ? std::optional(candidates[current]) : std::nullopt; }
    bool Start(Settings settings, double fps, double now)
    {
        if (Active() || state == State::Restoring || !std::isfinite(settings.scale) || settings.scale < .25f ||
            settings.scale > 2 || settings.cadence < 1 || settings.cadence > 4 || !std::isfinite(fps) || fps < 1 ||
            fps > 1000 || !std::isfinite(now))
            return false;
        original = settings;
        candidates = Candidates(settings);
        results.clear();
        samples.clear();
        seen.clear();
        context.reset();
        current = settled = 0;
        contract = settledThrough = 0;
        began = settleBegan = now;
        target = fps;
        refusal.clear();
        aborted = false;
        resultValid = true;
        token = ++serial;
        state = State::Settling;
        return true;
    }
    void Cancel(Abort why)
    {
        if (!Active())
            return;
        resultValid = false;
        abort = why;
        aborted = true;
        token = 0;
        state = State::Restoring;
    }
    // Restoration is acknowledgement of override removal, never a Config write.
    void Restored()
    {
        if (state == State::Restoring)
            state = aborted ? State::Aborted : State::Done;
    }
    void Observe(const Context& value)
    {
        if (!Active())
            return;
        if (value.original != original)
        {
            Cancel(Abort::Unsupported);
            return;
        }
        if (!context)
        {
            context = value;
            return;
        }
        if (value.device != context->device)
            Cancel(Abort::DeviceLost);
        else if (value.route != context->route || value.passes != context->passes || value.afterRR != context->afterRR)
            Cancel(Abort::RouteChanged);
        else if (value.width != context->width || value.height != context->height ||
                 value.renderWidth != context->renderWidth || value.renderHeight != context->renderHeight)
            Cancel(Abort::Resized);
    }
    void Refuse(const std::string& reason)
    {
        if (Active())
            refusal = reason;
    }
    void Tick(double now)
    {
        if (!Active() || !std::isfinite(now) || now < began || now - began < TimeoutMs)
            return;
        Result result;
        result.settings = candidates[current];
        result.count = samples.size();
        result.reason = refusal.empty() ? "No complete confirmed timing window" : refusal;
        results.push_back(result);
        Next(now);
    }
    void Accept(const Sample& sample, double now)
    {
        Tick(now);
        if (!Active() || !context || sample.token != token || !sample.id || !sample.confirmed || sample.reset ||
            sample.capture || !std::isfinite(sample.gpuMs) || !std::isfinite(sample.frameMs) || sample.gpuMs <= 0 ||
            sample.frameMs <= 0 || sample.frameMs > 1000 || !sample.contract || !std::isfinite(now) || now < began ||
            std::find(seen.begin(), seen.end(), sample.id) != seen.end())
            return;
        // Completion order can differ from recording order. A late settling recording cannot
        // enter the measured window merely because its fence completed after the transition.
        if (state == State::Sampling && sample.id <= settledThrough)
            return;
        // Bounded even under a very high frame rate during the minimum wall-clock settle interval.
        if (seen.size() >= 4096)
        {
            Cancel(Abort::Unsupported);
            return;
        }
        seen.push_back(sample.id);
        if (state == State::Settling)
        {
            settledThrough = std::max(settledThrough, sample.id);
            if (contract && contract != sample.contract)
            {
                settled = 0;
                settleBegan = now;
            }
            contract = sample.contract;
            if (++settled >= SettleCount && now - settleBegan >= SettleMs)
                state = State::Sampling;
            return;
        }
        if (sample.contract != contract)
        {
            Cancel(Abort::Unsupported);
            return;
        }
        samples.push_back(sample);
        if (samples.size() == SampleCount)
        {
            results.push_back(Summarize(candidates[current], samples));
            Next(now);
        }
    }
};
} // namespace DlssNr::Advisor
