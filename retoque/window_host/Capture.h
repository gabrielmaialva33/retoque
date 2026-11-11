// Windows Graphics Capture of one top-level window into the host's D3D11 device. The WinRT types stay in Capture.cpp.
#pragma once

#include "Geometry.h"

#include <memory>
#include <string>

namespace RetoqueWindowHost
{
struct CapturedFrame
{
    ID3D11Texture2D* texture = nullptr; // the pool's surface; valid until Retire releases its frame
    Geometry::Size content;             // the frame's ContentSize
    int64_t time = 0;                   // SystemRelativeTime, 100 ns
    uint64_t superseded = 0;            // older frames dropped by this poll (newest wins)
    uint64_t token = 0;                 // hand back to Retire
};

class Capture
{
  public:
    Capture();
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    static bool Supported(std::string& why);

    // Starts capturing the window; the wake event is signalled when a frame arrives or the item closes.
    bool Start(HWND window, ID3D11Device* device, bool cursor, HANDLE wake, std::string& why);

    // The newest frame in the pool, if any. A content size change recreates the pool and returns no frame.
    bool Poll(CapturedFrame& frame, std::string& why);

    // A polled frame stays out of the pool until the D3D11 copy that read it is known complete (the caller proves it
    // with its fence), so WGC never writes a surface the GPU may still be reading.
    void Hold(uint64_t token, uint64_t fenceValue);
    void Retire(uint64_t completedFenceValue);
    size_t Held() const;

    bool ItemClosed() const;

    // Stops the session. Frames still held are kept until Retire proves them, or forever (Abandon).
    void Stop();
    void Abandon();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace RetoqueWindowHost
