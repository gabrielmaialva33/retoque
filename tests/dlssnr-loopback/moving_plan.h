// Test-only scene state/clock transport. Portable so host tests execute the actual reader.
#pragma once

#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace MovingPlan
{
struct Frame
{
    unsigned frame, poseFrame, previousPoseFrame;
    bool reset;
    double timeMs, frameTimeMs;
};

inline void Need(bool condition, const char* reason)
{
    if (!condition)
        throw std::runtime_error(reason);
}

inline bool End(std::istringstream& stream)
{
    stream >> std::ws;
    return stream.eof();
}

inline std::vector<Frame> Load(const char* path, unsigned expectedFrames)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Need(file.is_open(), "moving plan open failed");
    const std::streamoff size = file.tellg();
    Need(size > 0 && size <= 2 * 1024 * 1024, "moving plan exceeds byte bound or is empty");
    file.seekg(0);
    std::string contents(static_cast<size_t>(size), '\0');
    Need(static_cast<bool>(file.read(contents.data(), static_cast<std::streamsize>(size))),
         "moving plan was truncated while reading");
    Need(file.peek() == std::char_traits<char>::eof(), "moving plan grew while reading");
    std::istringstream input(contents);
    std::string line, magic;
    Need(static_cast<bool>(std::getline(input, line)), "moving plan header missing");
    std::istringstream header(line);
    long long version = 0, count = 0;
    Need(static_cast<bool>(header >> magic >> version >> count) && End(header) && magic == "moving-plan" &&
             version == 1 && count > 0 && count <= 10000 && count == expectedFrames,
         "moving plan header/version/frame count mismatch");
    std::vector<Frame> result;
    result.reserve(static_cast<size_t>(count));
    for (long long i = 0; i < count; ++i)
    {
        Need(static_cast<bool>(std::getline(input, line)), "moving plan frame missing");
        std::istringstream fields(line);
        long long ordinal = -1, pose = -1, previous = -1, reset = -1;
        double time = 0, delta = 0;
        Need(static_cast<bool>(fields >> ordinal >> pose >> previous >> reset >> time >> delta) && End(fields),
             "moving plan frame has malformed fields");
        Need(ordinal == i && pose >= 0 && pose <= 1000000 && previous >= 0 && previous <= 1000000 &&
                 (reset == 0 || reset == 1),
             "moving plan frame identity/pose/reset out of range");
        Need(std::isfinite(time) && std::isfinite(delta) && time >= 0 && time <= 3600000 && delta > 0 && delta <= 1000,
             "moving plan clock out of range");
        if (i == 0)
            Need(reset == 1 && time == 0, "moving plan must begin with a reset at time zero");
        else
        {
            Need(time > result.back().timeMs && std::abs(time - result.back().timeMs - delta) <= 1e-4,
                 "moving plan clock/delta mismatch");
        }
        Need(reset ? previous == pose : !result.empty() && previous == result.back().poseFrame,
             "moving plan previous pose is not the actual rendered history");
        result.push_back({ static_cast<unsigned>(ordinal), static_cast<unsigned>(pose), static_cast<unsigned>(previous),
                           reset != 0, time, delta });
    }
    input >> std::ws;
    Need(input.eof(), "moving plan has trailing frames/data");
    return result;
}
} // namespace MovingPlan
