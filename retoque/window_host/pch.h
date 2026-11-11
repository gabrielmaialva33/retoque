// RetoqueWindowHost's common Windows headers. Every translation unit of the host starts with this; the pure rules
// (Geometry.h, Policy.h, Pacing.h, Protocol.h) never include it.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>

using Microsoft::WRL::ComPtr;
